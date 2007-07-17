/* OpenSceneGraph example, osgterrain.
*
*  Permission is hereby granted, free of charge, to any person obtaining a copy
*  of this software and associated documentation files (the "Software"), to deal
*  in the Software without restriction, including without limitation the rights
*  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*  copies of the Software, and to permit persons to whom the Software is
*  furnished to do so, subject to the following conditions:
*
*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
*  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
*  THE SOFTWARE.
*/

#include <osg/Group>
#include <osg/Geode>
#include <osg/ShapeDrawable>
#include <osg/Texture2D>
#include <osg/PositionAttitudeTransform>
#include <osg/MatrixTransform>
#include <osg/CoordinateSystemNode>
#include <osg/ClusterCullingCallback>
#include <osg/ArgumentParser>

#include <osgDB/FileUtils>
#include <osgDB/ReadFile>

#include <osgUtil/GLObjectsVisitor>

#include <osgText/FadeText>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <osgGA/TrackballManipulator>
#include <osgGA/FlightManipulator>
#include <osgGA/DriveManipulator>
#include <osgGA/KeySwitchMatrixManipulator>
#include <osgGA/StateSetManipulator>
#include <osgGA/AnimationPathManipulator>
#include <osgGA/TerrainManipulator>

#include <osgTerrain/TerrainNode>
#include <osgTerrain/GeometryTechnique>
#include <osgTerrain/Layer>

#include <iostream>


class MasterOperation : public osg::Operation
{
public:

    typedef std::set<std::string> Files;
    typedef std::map<std::string, osg::ref_ptr<osg::Node> >  FilenameNodeMap;
    typedef std::vector< osg::ref_ptr<osg::Node> >  Nodes;


    MasterOperation(const std::string& filename):
        Operation("Master reading operation",true),
        _filename(filename)
    {
    }
    
    bool readMasterFile(Files& files) const
    {
        std::ifstream fin(_filename.c_str());
        if (fin)
        {
            osgDB::Input fr;
            fr.attach(&fin);

            bool readFilename;

            while(!fr.eof())
            {
                bool itrAdvanced = false;
                if (fr.matchSequence("file %s") || fr.matchSequence("file %w") )
                {
                    files.insert(fr[1].getStr());
                    fr += 2;
                    itrAdvanced = true;
                    readFilename = true;
                }

                if (!itrAdvanced)
                {
                    ++fr;
                }
            }
            
            return readFilename;
        }
        return false;
    }
    
    bool open(osg::Group* group)
    {
        Files files;
        readMasterFile(files);
        for(Files::iterator itr = files.begin();
            itr != files.end();
            ++itr)
        {
            osg::Node* model = osgDB::readNodeFile(*itr);
            if (model)
            {
                osg::notify(osg::NOTICE)<<"open: Loaded file "<<*itr<<std::endl;
                group->addChild(model);
                _existingFilenameNodeMap[*itr] = model;
            }
        }
        
        return true;
    }
    

    virtual void operator () (osg::Object* object)
    {
        // osg::notify(osg::NOTICE)<<"void operator ()"<<std::endl;
    
        Files files;
        readMasterFile(files);

        // osg::notify(osg::NOTICE)<<"void operator () files.size()="<<files.size()<<std::endl;

        Files newFiles;
        Files removedFiles;

        // find out which files are new, and which ones have been removed.
        {            
            OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_mutex);

            for(Files::iterator fitr = files.begin();
                fitr != files.end();
                ++fitr)
            {
                if (_existingFilenameNodeMap.count(*fitr)==0) newFiles.insert(*fitr);
            }

            for(FilenameNodeMap::iterator litr = _existingFilenameNodeMap.begin();
                litr != _existingFilenameNodeMap.end();
                ++litr)
            {
                if (files.count(litr->first)==0)
                {
                    removedFiles.insert(litr->first);
                }
            }
        }
        

        // first load the new files.
        FilenameNodeMap nodesToAdd;
        if (!newFiles.empty())
        {

            typedef std::vector< osg::ref_ptr<osg::GraphicsThread> > GraphicsThreads;
            GraphicsThreads threads;
        
            for(unsigned int i=0; i<= osg::GraphicsContext::getMaxContextID(); ++i)
            {
                osg::GraphicsContext* gc = osg::GraphicsContext::getCompileContext(i);
                osg::GraphicsThread* gt = gc ? gc->getGraphicsThread() : 0;
                if (gt) threads.push_back(gt);
            }

            bool requiresBarrier = false;
            
            for(Files::iterator nitr = newFiles.begin();
                nitr != newFiles.end();
                ++nitr)
            {
                osg::ref_ptr<osg::Node> loadedModel = osgDB::readNodeFile(*nitr);

                if (loadedModel.get())
                {
                    nodesToAdd[*nitr] = loadedModel;

                    osg::ref_ptr<osgUtil::GLObjectsOperation> compileOperation = new osgUtil::GLObjectsOperation(loadedModel.get());
                
                    for(GraphicsThreads::iterator gitr = threads.begin();
                        gitr != threads.end();
                        ++gitr)
                    {
                        (*gitr)->add( compileOperation.get() );
                        requiresBarrier = true;
                    }
                }
            }
            
            if (requiresBarrier)
            {
                _barrier = new osg::BarrierOperation(threads.size()+1);
                _barrier->setKeep(false);
                
                for(GraphicsThreads::iterator gitr = threads.begin();
                    gitr != threads.end();
                    ++gitr)
                {
                    (*gitr)->add(_barrier.get());
                }
                
                // wait for the graphics threads to complete.
                _barrier->block();
            }
        }
        
        bool requiresBlock = false;
        
        // pass the locally peppared data to MasterOperations shared data
        // so that updated thread can merge these changes with the main scene 
        // graph.  This merge is carried out via the update(..) method.
        if (!removedFiles.empty() || !nodesToAdd.empty())        
        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_mutex);
            _nodesToRemove.swap(removedFiles);
            _nodesToAdd.swap(nodesToAdd);
            requiresBlock = true;
        }

        // now block so we don't try to load anything till the new data has been merged
        // otherwise _existingFilenameNodeMap will get out of sync.
        if (requiresBlock)
        {
            _updatesMergedBlock.block();
        }
        else
        {
            OpenThreads::Thread::YieldCurrentThread();
        }

    }
    
    // merge the changes with the main scene graph.
    void update(osg::Group* scene)
    {
        OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_mutex);
        
        if (!_nodesToRemove.empty())
        {
            for(Files::iterator itr = _nodesToRemove.begin();
                itr != _nodesToRemove.end();
                ++itr)
            {
                FilenameNodeMap::iterator fnmItr = _existingFilenameNodeMap.find(*itr);
                if (fnmItr != _existingFilenameNodeMap.end())
                {
                    osg::notify(osg::NOTICE)<<"  removing "<<*itr<<std::endl;
                
                    scene->removeChild(fnmItr->second.get());
                    _existingFilenameNodeMap.erase(fnmItr);
                }
            }

            _nodesToRemove.clear();
        }
        
        if (!_nodesToAdd.empty())
        {
            for(FilenameNodeMap::iterator itr = _nodesToAdd.begin();
                itr != _nodesToAdd.end();
                ++itr)
            {
                scene->addChild(itr->second.get());
                _existingFilenameNodeMap[itr->first] = itr->second;
            }
            
            _nodesToAdd.clear();
        }

        _updatesMergedBlock.release();

    }
    
    // add release implementation so that any thread cancellation can
    // work even when blocks and barriers are used.
    virtual void release()
    {
        _updatesMergedBlock.release();
        if (_barrier.valid()) _barrier.release();
    }

    
    std::string                         _filename;
    
    OpenThreads::Mutex                  _mutex;
    FilenameNodeMap                     _existingFilenameNodeMap;
    Files                               _nodesToRemove;
    FilenameNodeMap                     _nodesToAdd;
    OpenThreads::Block                  _updatesMergedBlock;
    osg::ref_ptr<osg::BarrierOperation> _barrier;
};

class FilterHandler : public osgGA::GUIEventHandler 
{
public: 

    FilterHandler(osgTerrain::GeometryTechnique* gt):
        _gt(gt) {}

    bool handle(const osgGA::GUIEventAdapter &ea, osgGA::GUIActionAdapter &aa)
    {
        if (!_gt) return false;

        switch(ea.getEventType())
        {
        case(osgGA::GUIEventAdapter::KEYDOWN):
            {
                if (ea.getKey() == 'g')
                {
                    osg::notify(osg::NOTICE)<<"Gaussian"<<std::endl;
                    _gt->setFilterMatrixAs(osgTerrain::GeometryTechnique::GAUSSIAN);
                    return true;
                }
                else if (ea.getKey() == 's')
                {
                    osg::notify(osg::NOTICE)<<"Smooth"<<std::endl;
                    _gt->setFilterMatrixAs(osgTerrain::GeometryTechnique::SMOOTH);
                    return true;
                }
                else if (ea.getKey() == 'S')
                {
                    osg::notify(osg::NOTICE)<<"Sharpen"<<std::endl;
                    _gt->setFilterMatrixAs(osgTerrain::GeometryTechnique::SHARPEN);
                    return true;
                }
                else if (ea.getKey() == '+')
                {
                    _gt->setFilterWidth(_gt->getFilterWidth()*1.1);
                    osg::notify(osg::NOTICE)<<"Filter width = "<<_gt->getFilterWidth()<<std::endl;
                    return true;
                }
                else if (ea.getKey() == '-')
                {
                    _gt->setFilterWidth(_gt->getFilterWidth()/1.1);
                    osg::notify(osg::NOTICE)<<"Filter width = "<<_gt->getFilterWidth()<<std::endl;
                    return true;
                }
                else if (ea.getKey() == '>')
                {
                    _gt->setFilterBias(_gt->getFilterBias()+0.1);
                    osg::notify(osg::NOTICE)<<"Filter bias = "<<_gt->getFilterBias()<<std::endl;
                    return true;
                }
                else if (ea.getKey() == '<')
                {
                    _gt->setFilterBias(_gt->getFilterBias()-0.1);
                    osg::notify(osg::NOTICE)<<"Filter bias = "<<_gt->getFilterBias()<<std::endl;
                    return true;
                }
                break;
            }
        default:
            break;
        }
        return false;

    }

protected:

    osg::observer_ptr<osgTerrain::GeometryTechnique> _gt;

};



class LayerHandler : public osgGA::GUIEventHandler 
{
public: 

    LayerHandler(osgTerrain::Layer* layer):
        _layer(layer) {}

    bool handle(const osgGA::GUIEventAdapter &ea, osgGA::GUIActionAdapter &aa)
    {
        if (!_layer) return false;

        float scale = 1.2;

        switch(ea.getEventType())
        {
        case(osgGA::GUIEventAdapter::KEYDOWN):
            {
                if (ea.getKey() == 'q')
                {
                    _layer->transform(0.0, scale);
                    return true;
                }
                else if (ea.getKey() == 'a')
                {
                    _layer->transform(0.0, 1.0f/scale);
                    return true;
                }
                break;
            }
        default:
            break;
        }
        return false;

    }

protected:

    osg::observer_ptr<osgTerrain::Layer> _layer;
};

int main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);

    // construct the viewer.
    osgViewer::Viewer viewer(arguments);

    // set up the camera manipulators.
    {
        osg::ref_ptr<osgGA::KeySwitchMatrixManipulator> keyswitchManipulator = new osgGA::KeySwitchMatrixManipulator;

        keyswitchManipulator->addMatrixManipulator( '1', "Trackball", new osgGA::TrackballManipulator() );
        keyswitchManipulator->addMatrixManipulator( '2', "Flight", new osgGA::FlightManipulator() );
        keyswitchManipulator->addMatrixManipulator( '3', "Drive", new osgGA::DriveManipulator() );
        keyswitchManipulator->addMatrixManipulator( '4', "Terrain", new osgGA::TerrainManipulator() );

        std::string pathfile;
        char keyForAnimationPath = '5';
        while (arguments.read("-p",pathfile))
        {
            osgGA::AnimationPathManipulator* apm = new osgGA::AnimationPathManipulator(pathfile);
            if (apm || !apm->valid()) 
            {
                unsigned int num = keyswitchManipulator->getNumMatrixManipulators();
                keyswitchManipulator->addMatrixManipulator( keyForAnimationPath, "Path", apm );
                keyswitchManipulator->selectMatrixManipulator(num);
                ++keyForAnimationPath;
            }
        }

        viewer.setCameraManipulator( keyswitchManipulator.get() );
    }


    // add the state manipulator
    viewer.addEventHandler( new osgGA::StateSetManipulator(viewer.getCamera()->getOrCreateStateSet()) );

    // add the stats handler
    viewer.addEventHandler(new osgViewer::StatsHandler);

    // add the record camera path handler
    viewer.addEventHandler(new osgViewer::RecordCameraPathHandler);


    double x = 0.0;
    double y = 0.0;
    double w = 1.0;
    double h = 1.0;

    bool createBackgroundContextForCompiling = false;
    while (arguments.read("--bc")) { createBackgroundContextForCompiling = true; }

    bool createBackgroundThreadsForCompiling = false;
    while (arguments.read("--bt")) { createBackgroundContextForCompiling = true; createBackgroundThreadsForCompiling = true; }


    osg::ref_ptr<MasterOperation> masterOperation;
    std::string masterFilename;
    while(arguments.read("-m",masterFilename))
    {
        masterOperation = new MasterOperation(masterFilename);
    }
    

    osg::ref_ptr<osgTerrain::TerrainNode> terrain = new osgTerrain::TerrainNode;
    osg::ref_ptr<osgTerrain::Locator> locator = new osgTerrain::EllipsoidLocator(-osg::PI, -osg::PI*0.5, 2.0*osg::PI, osg::PI, 0.0);
    osg::ref_ptr<osgTerrain::ValidDataOperator> validDataOperator = new osgTerrain::NoDataValue(0.0);
    osg::ref_ptr<osgTerrain::Layer> lastAppliedLayer;

    unsigned int layerNum = 0;

    std::string filterName;

    bool readParameter = false;
    float minValue, maxValue;
    float scale = 1.0f;
    float offset = 0.0f;

    int pos = 1;
    while(pos<arguments.argc())
    {
        readParameter = false;
        std::string filename;
        
        if (arguments.read(pos, "--layer",layerNum)) 
        {
            osg::notify(osg::NOTICE)<<"Set layer number to "<<layerNum<<std::endl;
            readParameter = true;
        }

        else if (arguments.read(pos, "-b"))
        {
            terrain->setTreatBoundariesToValidDataAsDefaultValue(true);
        }
        
        else if (arguments.read(pos, "-e",x,y,w,h))
        {
            // define the extents.
            locator = new osgTerrain::EllipsoidLocator(x,y,w,h,0);
            readParameter = true;
        }

        else if (arguments.read(pos, "--transform",offset, scale) || arguments.read(pos, "-t",offset, scale))
        {
            // define the extents.
            readParameter = true;
        }

        else if (arguments.read(pos, "--cartesian",x,y,w,h))
        {
            // define the extents.
            locator = new osgTerrain::CartesianLocator(x,y,w,h,0);
            readParameter = true;
        }

        else if (arguments.read(pos, "--hf",filename))
        {
            readParameter = true;
            
            osg::notify(osg::NOTICE)<<"--hf "<<filename<<std::endl;

            osg::ref_ptr<osg::HeightField> hf = osgDB::readHeightFieldFile(filename);
            if (hf.valid())
            {
                osg::ref_ptr<osgTerrain::HeightFieldLayer> hfl = new osgTerrain::HeightFieldLayer;
                hfl->setHeightField(hf.get());
                
                hfl->setLocator(locator.get());
                hfl->setValidDataOperator(validDataOperator.get());
                
                if (offset!=0.0f || scale!=1.0f)
                {
                    hfl->transform(offset,scale);
                }
                
                terrain->setElevationLayer(hfl.get());
                
                lastAppliedLayer = hfl.get();
                
                osg::notify(osg::NOTICE)<<"created osgTerrain::HeightFieldLayer"<<std::endl;
            }
            else
            {
                osg::notify(osg::NOTICE)<<"failed to create osgTerrain::HeightFieldLayer"<<std::endl;
            }
            
            scale = 1.0f;
            offset = 0.0f;
            
        }

        else if (arguments.read(pos, "-d",filename) || arguments.read(pos, "--elevation-image",filename))
        {
            readParameter = true;
            osg::notify(osg::NOTICE)<<"--elevation-image "<<filename<<std::endl;

            osg::ref_ptr<osg::Image> image = osgDB::readImageFile(filename);
            if (image.valid())
            {
                osg::ref_ptr<osgTerrain::ImageLayer> imageLayer = new osgTerrain::ImageLayer;
                imageLayer->setImage(image.get());
                imageLayer->setLocator(locator.get());
                imageLayer->setValidDataOperator(validDataOperator.get());
                
                if (offset!=0.0f || scale!=1.0f)
                {
                    imageLayer->transform(offset,scale);
                }
                
                terrain->setElevationLayer(imageLayer.get());
                
                lastAppliedLayer = imageLayer.get();

                osg::notify(osg::NOTICE)<<"created Elevation osgTerrain::ImageLayer"<<std::endl;
            }
            else
            {
                osg::notify(osg::NOTICE)<<"failed to create osgTerrain::ImageLayer"<<std::endl;
            }

            scale = 1.0f;
            offset = 0.0f;
            
        }
        
        else if (arguments.read(pos, "-c",filename) || arguments.read(pos, "--image",filename))
        {
            readParameter = true;
            osg::notify(osg::NOTICE)<<"--image "<<filename<<" x="<<x<<" y="<<y<<" w="<<w<<" h="<<h<<std::endl;

            osg::ref_ptr<osg::Image> image = osgDB::readImageFile(filename);
            if (image.valid())
            {
                osg::ref_ptr<osgTerrain::ImageLayer> imageLayer = new osgTerrain::ImageLayer;
                imageLayer->setImage(image.get());
                imageLayer->setLocator(locator.get());
                imageLayer->setValidDataOperator(validDataOperator.get());
                
                if (offset!=0.0f || scale!=1.0f)
                {
                    imageLayer->transform(offset,scale);
                }

                terrain->setColorLayer(layerNum, imageLayer.get());

                lastAppliedLayer = imageLayer.get();

                osg::notify(osg::NOTICE)<<"created Color osgTerrain::ImageLayer"<<std::endl;
            }
            else
            {
                osg::notify(osg::NOTICE)<<"failed to create osgTerrain::ImageLayer"<<std::endl;
            }

            scale = 1.0f;
            offset = 0.0f;
            
        }

        else if (arguments.read(pos, "--filter",filterName))
        {
            readParameter = true;
            
            if (filterName=="NEAREST")
            {
                osg::notify(osg::NOTICE)<<"--filter "<<filterName<<std::endl;
                terrain->setColorFilter(layerNum, osgTerrain::TerrainNode::NEAREST);
            }
            else if (filterName=="LINEAR") 
            {
                osg::notify(osg::NOTICE)<<"--filter "<<filterName<<std::endl;
                terrain->setColorFilter(layerNum, osgTerrain::TerrainNode::LINEAR);
            }
            else
            {
                osg::notify(osg::NOTICE)<<"--filter "<<filterName<<" unrecognized filter name, please use LINEAER or NEAREST."<<std::endl;
            }            
            
        }

        else if (arguments.read(pos, "--tf",minValue, maxValue))
        {
            readParameter = true;

            osg::ref_ptr<osg::TransferFunction1D> tf = new osg::TransferFunction1D;
            
            tf->setInputRange(minValue, maxValue);
            
            tf->allocate(6);
            tf->setValue(0, osg::Vec4(1.0,1.0,1.0,1.0));
            tf->setValue(1, osg::Vec4(1.0,0.0,1.0,1.0));
            tf->setValue(2, osg::Vec4(1.0,0.0,0.0,1.0));
            tf->setValue(3, osg::Vec4(1.0,1.0,0.0,1.0));
            tf->setValue(4, osg::Vec4(0.0,1.0,1.0,1.0));
            tf->setValue(5, osg::Vec4(0.0,1.0,0.0,1.0));
            
            osg::notify(osg::NOTICE)<<"--tf "<<minValue<<" "<<maxValue<<std::endl;

            terrain->setColorTransferFunction(layerNum, tf.get());
        }
        else
        {
            ++pos;
        }

    }
    

    osg::ref_ptr<osg::Group> scene = new osg::Group;

    if (terrain.valid() && (terrain->getElevationLayer() || terrain->getColorLayer(0)))
    {
        osg::notify(osg::NOTICE)<<"Terrain created"<<std::endl;
    
        scene->addChild(terrain.get());

        osg::ref_ptr<osgTerrain::GeometryTechnique> geometryTechnique = new osgTerrain::GeometryTechnique;
        terrain->setTerrainTechnique(geometryTechnique.get());
        viewer.addEventHandler(new FilterHandler(geometryTechnique.get()));
        viewer.addEventHandler(new LayerHandler(lastAppliedLayer.get()));
    }

    if (masterOperation.valid())
    {
        osg::notify(osg::NOTICE)<<"Master operation created"<<std::endl;

        masterOperation->open(scene.get());
    }
    
    if (scene->getNumChildren()==0)
    {
        osg::notify(osg::NOTICE)<<"No model created, please specify terrain or master file on command line."<<std::endl;
        return 0;
    }
    
    viewer.setSceneData(scene.get());


    // start operation thread if a master file has been used.
    osg::ref_ptr<osg::OperationThread> operationThread;
    if (masterOperation.valid()) 
    {
        operationThread = new osg::OperationThread;
        operationThread->startThread();
        operationThread->add(masterOperation.get());
    }

    // realize the graphics windows.
    viewer.realize();
    
    // set up any compile contexts that are required.
    if (createBackgroundContextForCompiling)
    {
    
        int numProcessors = OpenThreads::GetNumberOfProcessors();
        int processNum = 0;

        for(unsigned int i=0; i<= osg::GraphicsContext::getMaxContextID(); ++i)
        {
            osg::GraphicsContext* gc = osg::GraphicsContext::getOrCreateCompileContext(i);

            if (gc && createBackgroundThreadsForCompiling)
            {
                gc->createGraphicsThread();
                gc->getGraphicsThread()->setProcessorAffinity(processNum % numProcessors);
                gc->getGraphicsThread()->startThread();
                
                ++processNum;
            }
        }
    }


    // run main loop, with syncing with masterOperation
    while (!viewer.done())
    {
        viewer.advance();
        viewer.eventTraversal();
        viewer.updateTraversal();
        
        if (masterOperation.valid()) masterOperation->update(scene.get());
        
        viewer.frame();
    }
    
    // kill the operation thread
    operationThread = 0;

}
