// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

// ospray
#include "ospray/lights/Light.h"
#include "ospray/common/Data.h"
#include "ospray/common/Core.h"
#include "ospray/render/volume/RaycastVolumeRenderer.h"
// ispc exports
#include "RaycastVolumeRenderer_ispc.h"
#include "RaycastVolumeRendererMaterial_ispc.h"
#if EXP_DATA_PARALLEL
# include "ospray/mpi/DistributedFrameBuffer.h"
# include "ospray/volume/DataDistributedBlockedVolume.h"
# include "ospray/render/LoadBalancer.h"
#endif

#define PREALLOC_ALL_TILES 0
#define TILE_CACHE_SAFE_MUTEX 0

namespace ospray {

  RaycastVolumeRenderer::Material::Material()
  {
    ispcEquivalent = ispc::RaycastVolumeRendererMaterial_create(this);
  }

  void RaycastVolumeRenderer::Material::commit()
  {
    Kd = getParam3f("color", getParam3f("kd", getParam3f("Kd", vec3f(1.0f))));
    volume = (Volume *)getParamObject("volume", NULL);

    ispc::RaycastVolumeRendererMaterial_set(getIE(), (const ispc::vec3f&)Kd, 
                                            volume ? volume->getIE() : NULL);
  }

#if EXP_DATA_PARALLEL

  struct CacheForBlockTiles {
#if PREALLOC_ALL_TILES
    int *tileUsed;
#endif
    CacheForBlockTiles(size_t numBlocks) 
      : numBlocks(numBlocks), blockTile(new Tile *[numBlocks])
    { 
#if PREALLOC_ALL_TILES
      for (int i=0;i<numBlocks;i++) blockTile[i] = allocTile();
      tileUsed = new int[numBlocks];
      for (int i=0;i<numBlocks;i++) tileUsed[i] = false;
#else
      for (int i=0;i<numBlocks;i++) blockTile[i] = NULL; 
#endif
    }

    Tile *allocTile() 
    {
      Tile *tile = new Tile;
      for (int i=0;i<TILE_SIZE*TILE_SIZE;i++) tile->r[i] = 0.f;
      for (int i=0;i<TILE_SIZE*TILE_SIZE;i++) tile->g[i] = 0.f;
      for (int i=0;i<TILE_SIZE*TILE_SIZE;i++) tile->b[i] = 0.f;
      for (int i=0;i<TILE_SIZE*TILE_SIZE;i++) tile->a[i] = 0.f;
      for (int i=0;i<TILE_SIZE*TILE_SIZE;i++) tile->z[i] = std::numeric_limits<float>::infinity();
      return tile;
    }

    ~CacheForBlockTiles() 
    { 
      for (int i=0;i<numBlocks;i++)
        if (blockTile[i]) delete blockTile[i];
#if PREALLOC_ALL_TILES
      delete[] tileUsed;
#endif
      delete[] blockTile;
    }
    Tile *getTileForBlock(size_t blockID) 
    {
#if PREALLOC_ALL_TILES
      tileUsed[blockID] = true;
#endif


#if TILE_CACHE_SAFE_MUTEX
      mutex.lock();
      Tile *tile = blockTile[blockID];
      if (tile == NULL) {
        blockTile[blockID] = tile = allocTile();
      }
      mutex.unlock();
      return tile;
#else
      Tile *tile = blockTile[blockID];
      if (tile != NULL) return tile;
      mutex.lock();
      tile = blockTile[blockID];
      if (tile == NULL) {
        blockTile[blockID] = tile = allocTile();
      }
      mutex.unlock();
      return tile;
#endif
    }
    
    Mutex mutex;
    size_t numBlocks;
    Tile *volatile *blockTile;
  };

  /*! extern exported function so even ISPC code can access this cache */
  extern "C" Tile *CacheForBlockTiles_getTileForBlock(CacheForBlockTiles *cache, uint32 blockID) 
  { return cache->getTileForBlock(blockID); }

  struct DPRenderTask : public ospray::Task {

    Ref<Renderer>     renderer;
    Ref<FrameBuffer>  fb;
    size_t            numTiles_x;
    size_t            numTiles_y;
    uint32            channelFlags;
    int32             workerRank;
    const DataDistributedBlockedVolume *dpv;

    DPRenderTask(int workerRank) 
      : workerRank(workerRank) 
    {
    }
    
    virtual void run(size_t taskIndex)
    {
      const size_t tileID = taskIndex;
      Tile bgTile, fgTile;
      const size_t tile_y = taskIndex / numTiles_x;
      const size_t tile_x = taskIndex - tile_y*numTiles_x;
      bgTile.region.lower.x = tile_x * TILE_SIZE;
      bgTile.region.lower.y = tile_y * TILE_SIZE;
      bgTile.region.upper.x = std::min(bgTile.region.lower.x+TILE_SIZE,fb->size.x);
      bgTile.region.upper.y = std::min(bgTile.region.lower.y+TILE_SIZE,fb->size.y);
      bgTile.fbSize = fb->size;
      bgTile.rcp_fbSize = rcp(vec2f(bgTile.fbSize));
      bgTile.generation = 0;
      bgTile.children = 0;

      fgTile.region = bgTile.region;
      fgTile.fbSize = bgTile.fbSize;
      fgTile.rcp_fbSize = bgTile.rcp_fbSize;
      fgTile.generation = 0;
      fgTile.children = 0;

      size_t numBlocks = dpv->numDDBlocks;
      CacheForBlockTiles blockTileCache(numBlocks);
      // for (int i=0;i<numBlocks;i++)
      //   PRINT(dpv->ddBlock[i].bounds);
      bool blockWasVisible[numBlocks];
      for (int i=0;i<numBlocks;i++)
        blockWasVisible[i] = false;
      bool itIsIThatHasToRenderForeAndBackOnThisTile
        = (taskIndex % core::getWorkerCount()) == core::getWorkerRank();
      ispc::DDDVRRenderer_renderTile
        (renderer->getIE(),(ispc::Tile&)fgTile,(ispc::Tile&)bgTile,
         &blockTileCache,numBlocks,dpv->ddBlock,blockWasVisible,tileID,
         ospray::core::getWorkerRank(),itIsIThatHasToRenderForeAndBackOnThisTile);

      if (itIsIThatHasToRenderForeAndBackOnThisTile) {
        // this is a tile owned by me - i'm responsible for writing
        // generaition #0, and telling the fb how many more tiles will
        // be coming in generation #1
        
        size_t totalBlocksInTile=0;
        for (int blockID=0;blockID<numBlocks;blockID++) 
          if (blockWasVisible[blockID])
            totalBlocksInTile++;

        size_t nextGenTiles
          = 1 /* expect one additional tile for background tile. */
          + totalBlocksInTile /* plus how many blocks map to this
                                 tile, IN TOTAL (ie, INCLUDING blocks
                                 on other nodes)*/;
        // printf("rank %i total tiles in tile %i is %i\n",core::getWorkerRank(),taskIndex,nextGenTiles);

        // set background tile
        bgTile.generation = 0;
        bgTile.children = nextGenTiles;
        fb->setTile(bgTile);

        // set foreground tile
        fgTile.generation = 1;
        fgTile.children = 0; //nextGenTiles-1;
        fb->setTile(fgTile);
        // all other tiles for gen #1 will be set below, no matter whether it's mine or not
      }

      // now, send all block cache tiles that were generated on this
      // node back to master, too. for this, it doesn't matter if this
      // is our tile or not; it's the job of the owner of this tile to
      // tell the DFB how many tiles will arrive for the final thile
      // _across_all_clients_, but we only have to send ours (assuming
      // that all clients together send exactly as many as the owner
      // told the DFB to expect)
      for (int blockID=0;blockID<numBlocks;blockID++) {
#if PREALLOC_ALL_TILES
        if (!blockTileCache.tileUsed[blockID])
          continue;
#endif
        Tile *tile = blockTileCache.blockTile[blockID];
        if (tile == NULL) 
          continue;
        tile->region = bgTile.region;
        tile->fbSize = bgTile.fbSize;
        tile->rcp_fbSize = bgTile.rcp_fbSize;
        tile->generation = 1;
        tile->children = 0; //nextGenTile-1;

        for (int i=0;i<TILE_SIZE*TILE_SIZE;i++)
          tile->r[i] = float((blockID*3*7) % 11) / 11.f;

        fb->setTile(*tile);
      }
    }
    
    virtual ~DPRenderTask() {}
  };

  
  /*! try if we are running in data-parallel mode, and if
    data-parallel is even required. if not (eg, if there's no
    data-parallel volumes in the scene) return NULL and render only
    in regular mode; otherwise, compute some precomputations and
    return pointer to that (which'll swtich the main renderframe fct
    to render data parallel) */
  void RaycastVolumeRenderer::renderFrame(FrameBuffer *fb, const uint32 channelFlags)
  {
    int workerRank = ospray::core::getWorkerRank();

    std::vector<const DataDistributedBlockedVolume *> ddVolumeVec;
    for (int volumeID=0;volumeID<model->volume.size();volumeID++) {
      const DataDistributedBlockedVolume *ddv
        = dynamic_cast<const DataDistributedBlockedVolume*>(model->volume[volumeID].ptr);
      if (!ddv) continue;
      ddVolumeVec.push_back(ddv);
    }

    if (ddVolumeVec.empty()) {
      cout << "no data parallel volumes, rendering in traditional raycast_volume_render mode" << endl;

      Renderer::renderFrame(fb,channelFlags);
      return;
    }

    // =======================================================
    // OK, we _need_ data-parallel rendering ....
    std::cout << "#dvr: at least one dp volume"
      " -> needs data-parallel rendering ..." << std::endl;

    // check if we're even in mpi parallel mode (can't do
    // data-parallel otherwise)
    if (!ospray::core::isMpiParallel()) 
      throw std::runtime_error("#dvr: need data-parallel rendering, "
                               "but not running in mpi mode!?");

    // switch (distributed) frame buffer into compositing mode
    DistributedFrameBuffer *dfb = dynamic_cast<DistributedFrameBuffer *>(fb);
    if (!dfb)
      throw std::runtime_error("OSPRay data parallel rendering error. "
                               "this is a data-parallel scene, but we're "
                               "not using the distributed frame buffer!?");
    dfb->setFrameMode(DistributedFrameBuffer::ALPHA_BLENDING);


    // note: we can NEVER be the master, since the master doesn't even
    // have an instance of this renderer class - 
    assert(workerRank >= 0);
    
    Renderer::beginFrame(fb);

    dfb->startNewFrame();

    if (ddVolumeVec.size() > 1)
      /* note: our assumption below is that all blocks together are
         contiguous, and fill a convex region (ie, any point on a
         given ray is either entirely before any block, entirely
         behind any block, or inside one of the blocks) - if we have
         multiple data distributed volumes that is no longer the case,
         so we're not currently supporting this ... */
      throw std::runtime_error("currently supporting only ONE data parallel volume in scene");

    // create the render task
    Ref<DPRenderTask> renderTask = new DPRenderTask(workerRank);
    renderTask->fb = fb;
    renderTask->renderer = this;
    renderTask->numTiles_x = divRoundUp(dfb->size.x,TILE_SIZE);
    renderTask->numTiles_y = divRoundUp(dfb->size.y,TILE_SIZE);
    renderTask->channelFlags = channelFlags;
    renderTask->dpv = ddVolumeVec[0];
    /*! iw: using a local sync event for now; "in theory" we should be
      able to attach something like a sync event to the frame
      buffer, just trigger the task here, and let somebody else sync
      on the framebuffer once it is needed; alas, I'm currently
      running into some issues with the embree taks system when
      trying to do so, and thus am reverting to this
      fully-synchronous version for now */
    size_t numTilesTotal = renderTask->numTiles_x*renderTask->numTiles_y;
    renderTask->schedule(numTilesTotal);
    renderTask->wait();
      
    dfb->waitUntilFinished();
    Renderer::endFrame(NULL,channelFlags);
  }


#endif


  void RaycastVolumeRenderer::commit() 
  {
    // Create the equivalent ISPC RaycastVolumeRenderer object.
    if (ispcEquivalent == NULL) 
      ispcEquivalent = ispc::RaycastVolumeRenderer_createInstance();

    // Get the background color.
    vec3f bgColor = getParam3f("bgColor", vec3f(1.f));

    // Set the background color.
    ispc::RaycastVolumeRenderer_setBackgroundColor(ispcEquivalent, (const ispc::vec3f&) bgColor);

    // Set the lights if any.
    Data *lightsData = (Data *)getParamData("lights", NULL);

    lights.clear();

    if (lightsData)
      for (size_t i=0; i<lightsData->size(); i++)
        lights.push_back(((Light **)lightsData->data)[i]->getIE());

    ispc::RaycastVolumeRenderer_setLights(ispcEquivalent, 
                                          lights.empty() ? NULL : &lights[0], 
                                          lights.size());

    // Initialize state in the parent class, must be called after the ISPC object is created.
    Renderer::commit();
  }

  // A renderer type for volumes with embedded surfaces.
  OSP_REGISTER_RENDERER(RaycastVolumeRenderer, raycast_volume_renderer);
  OSP_REGISTER_RENDERER(RaycastVolumeRenderer, dvr);

} // ::ospray

