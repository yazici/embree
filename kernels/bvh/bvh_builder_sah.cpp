// ======================================================================== //
// Copyright 2009-2016 Intel Corporation                                    //
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

#include "bvh.h"
#include "bvh_builder.h"

#include "../builders/primrefgen.h"
#include "../builders/presplit.h"

#include "../geometry/bezier1v.h"
#include "../geometry/bezier1i.h"
#include "../geometry/linei.h"
#include "../geometry/triangle.h"
#include "../geometry/trianglev.h"
#include "../geometry/trianglei.h"
#include "../geometry/trianglev_mb.h"
#include "../geometry/trianglei_mb.h"
#include "../geometry/quadv.h"
#include "../geometry/quadi.h"
#include "../geometry/quadi_mb.h"
#include "../geometry/object.h"

#include "../common/state.h"

#define PROFILE 0
#define PROFILE_RUNS 20

namespace embree
{
  namespace isa
  {
    MAYBE_UNUSED static const float travCost = 1.0f;
    MAYBE_UNUSED static const float defaultPresplitFactor = 1.2f;

    typedef FastAllocator::ThreadLocal2 Allocator;

    template<int N, typename Primitive>
    struct CreateLeaf
    {
      typedef BVHN<N> BVH;

      __forceinline CreateLeaf (BVH* bvh, PrimRef* prims) : bvh(bvh), prims(prims) {}
      
      __forceinline size_t operator() (const BVHBuilderBinnedSAH::BuildRecord& current, Allocator* alloc)
      {
        size_t n = current.prims.size();
        size_t items = Primitive::blocks(n);
        size_t start = current.prims.begin();
        Primitive* accel = (Primitive*) alloc->alloc1->malloc(items*sizeof(Primitive),BVH::byteNodeAlignment);
        typename BVH::NodeRef node = BVH::encodeLeaf((char*)accel,items);
        for (size_t i=0; i<items; i++) {
          accel[i].fill(prims,start,current.prims.end(),bvh->scene,false);
        }
        *current.parent = node;
	return n;
      }

      BVH* bvh;
      PrimRef* prims;
    };


    template<int N, typename Primitive>
    struct CreateLeafQuantized
    {
      typedef BVHN<N> BVH;

      __forceinline CreateLeafQuantized (BVH* bvh, PrimRef* prims) : bvh(bvh), prims(prims) {}
      
      __forceinline size_t operator() (const BVHBuilderBinnedSAH::BuildRecord& current, Allocator* alloc)
      {
        size_t n = current.prims.size();
        size_t items = Primitive::blocks(n);
        size_t start = current.prims.begin();
        // todo alloc0/1 or alloc
        Primitive* accel = (Primitive*) alloc->alloc0->malloc(items*sizeof(Primitive),BVH::byteNodeAlignment);
        typename BVH::NodeRef node = BVH::encodeLeaf((char*)accel,items);
        for (size_t i=0; i<items; i++) {
          accel[i].fill(prims,start,current.prims.end(),bvh->scene,false);
        }

        *current.parent = node;
	return n;
      }

      BVH* bvh;
      PrimRef* prims;
    };


    template<int N, typename Primitive>
    struct CreateLeafSpatial
    {
      typedef BVHN<N> BVH;

      __forceinline CreateLeafSpatial (BVH* bvh, PrimRef* prims0) : bvh(bvh), prims0(prims0) {}
      
      __forceinline size_t operator() (const BVHBuilderBinnedFastSpatialSAH::BuildRecord& current, Allocator* alloc)
      {
        PrimRef* const source = prims0;

        size_t n = current.prims.size();


        size_t items = Primitive::blocks(n);

        size_t start = current.prims.begin();

        // remove number of split encoding
        for (size_t i=0; i<n; i++) 
          source[start+i].lower.a &= 0x00FFFFFF;

        Primitive* accel = (Primitive*) alloc->alloc1->malloc(items*sizeof(Primitive),BVH::byteNodeAlignment);
        typename BVH::NodeRef node = BVH::encodeLeaf((char*)accel,items);
        for (size_t i=0; i<items; i++) {
          accel[i].fill(source,start,current.prims.end(),bvh->scene,false);
        }
        *current.parent = node;
	return n;
      }

      BVH* bvh;
      PrimRef* prims0;
    };
    
    /************************************************************************************/ 
    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/

    template<int N, typename Mesh, typename Primitive>
    struct BVHNBuilderSAH : public Builder
    {
      typedef BVHN<N> BVH;
      BVH* bvh;
      Scene* scene;
      Mesh* mesh;
      mvector<PrimRef> prims;
      const size_t sahBlockSize;
      const float intCost;
      const size_t minLeafSize;
      const size_t maxLeafSize;
      const float presplitFactor;

      BVHNBuilderSAH (BVH* bvh, Scene* scene, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, const size_t mode)
        : bvh(bvh), scene(scene), mesh(nullptr), prims(scene->device), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)),
          presplitFactor((mode & MODE_HIGH_QUALITY) ? defaultPresplitFactor : 1.0f) {}


      BVHNBuilderSAH (BVH* bvh, Mesh* mesh, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, const size_t mode)
        : bvh(bvh), scene(nullptr), mesh(mesh), prims(bvh->device), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)),
          presplitFactor((mode & MODE_HIGH_QUALITY ) ? defaultPresplitFactor : 1.0f) {}

      // FIXME: shrink bvh->alloc in destructor here and in other builders too

      void build(size_t, size_t) 
      {
	/* skip build for empty scene */
        const size_t numPrimitives = mesh ? mesh->size() : scene->getNumPrimitives<Mesh,false>();
        if (numPrimitives == 0) {
          prims.clear();
          bvh->clear();
          return;
        }
        
        double t0 = bvh->preBuild(mesh ? "" : TOSTRING(isa) "::BVH" + toString(N) + "BuilderSAH");
        
#if PROFILE
        profile(2,PROFILE_RUNS,numPrimitives,[&] (ProfileTimer& timer) {
#endif

            /* create primref array */
            const size_t numSplitPrimitives = max(numPrimitives,size_t(presplitFactor*numPrimitives));
            prims.resize(numSplitPrimitives);
            PrimInfo pinfo = mesh ? 
              createPrimRefArray<Mesh>  (mesh ,prims,bvh->scene->progressInterface) : 
              createPrimRefArray<Mesh,false>(scene,prims,bvh->scene->progressInterface);

            /* pinfo might has zero size due to invalid geometry */
            if (unlikely(pinfo.size() == 0))
            {
              prims.clear();
              bvh->clear();
              return;
            }

            /* perform pre-splitting */
            if (presplitFactor > 1.0f) 
              pinfo = presplit<Mesh>(scene, pinfo, prims);
        
            /* call BVH builder */            
            bvh->alloc.init_estimate(pinfo.size()*sizeof(PrimRef));
            BVHNBuilder<N>::build(bvh,CreateLeaf<N,Primitive>(bvh,prims.data()),bvh->scene->progressInterface,prims.data(),pinfo,sahBlockSize,minLeafSize,maxLeafSize,travCost,intCost);

#if PROFILE
          }); 
#endif	

	/* clear temporary data for static geometry */
	bool staticGeom = mesh ? mesh->isStatic() : scene->isStatic();

	if (staticGeom) {
          prims.clear(); 
          bvh->shrink();
        }
	bvh->cleanup();
        bvh->postBuild(t0);
      }

      void clear() {
        prims.clear();
      }
    };

    /************************************************************************************/ 
    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/

    template<int N, typename Mesh, typename Primitive>
    struct BVHNBuilderSAHQuantized : public Builder
    {
      typedef BVHN<N> BVH;
      BVH* bvh;
      Scene* scene;
      Mesh* mesh;
      mvector<PrimRef> prims;
      const size_t sahBlockSize;
      const float intCost;
      const size_t minLeafSize;
      const size_t maxLeafSize;
      const float presplitFactor;

      BVHNBuilderSAHQuantized (BVH* bvh, Scene* scene, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, const size_t mode)
        : bvh(bvh), scene(scene), mesh(nullptr), prims(scene->device), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)),
          presplitFactor((mode & MODE_HIGH_QUALITY) ? defaultPresplitFactor : 1.0f) {}

      BVHNBuilderSAHQuantized (BVH* bvh, Mesh* mesh, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, const size_t mode)
        : bvh(bvh), scene(nullptr), mesh(mesh), prims(bvh->device), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)),
          presplitFactor((mode & MODE_HIGH_QUALITY) ? defaultPresplitFactor : 1.0f) {}

      // FIXME: shrink bvh->alloc in destructor here and in other builders too

      void build(size_t, size_t) 
      {
	/* skip build for empty scene */
        const size_t numPrimitives = mesh ? mesh->size() : scene->getNumPrimitives<Mesh,false>();
        if (numPrimitives == 0) {
          prims.clear();
          bvh->clear();
          return;
        }

        double t0 = bvh->preBuild(mesh ? "" : TOSTRING(isa) "::QBVH" + toString(N) + "BuilderSAH");

#if PROFILE
        profile(2,PROFILE_RUNS,numPrimitives,[&] (ProfileTimer& timer) {
#endif
            /* create primref array */
            const size_t numSplitPrimitives = max(numPrimitives,size_t(presplitFactor*numPrimitives));
            prims.resize(numSplitPrimitives);
            PrimInfo pinfo = mesh ? 
              createPrimRefArray<Mesh>  (mesh ,prims,bvh->scene->progressInterface) : 
              createPrimRefArray<Mesh,false>(scene,prims,bvh->scene->progressInterface);
        
            /* perform pre-splitting */
            if (presplitFactor > 1.0f) 
              pinfo = presplit<Mesh>(scene, pinfo, prims);
        
            /* call BVH builder */
            bvh->alloc.init_estimate(pinfo.size()*sizeof(PrimRef));
            BVHNBuilderQuantized<N>::build(bvh,CreateLeafQuantized<N,Primitive>(bvh,prims.data()),bvh->scene->progressInterface,prims.data(),pinfo,sahBlockSize,minLeafSize,maxLeafSize,travCost,intCost);

#if PROFILE
          }); 
#endif	

	/* clear temporary data for static geometry */
	bool staticGeom = mesh ? mesh->isStatic() : scene->isStatic();
	if (staticGeom) {
          prims.clear();
          bvh->shrink();
        }
	bvh->cleanup();
        bvh->postBuild(t0);
      }

      void clear() {
        prims.clear();
      }
    };

    /************************************************************************************/ 
    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/

    template<int N, typename Primitive>
    struct CreateMSMBlurLeaf
    {
      typedef BVHN<N> BVH;
      __forceinline CreateMSMBlurLeaf (BVH* bvh, PrimRef* prims, size_t time) : bvh(bvh), prims(prims), time(time) {}
      
      __forceinline LBBox3fa operator() (const BVHBuilderBinnedSAH::BuildRecord& current, Allocator* alloc)
      {
        size_t items = Primitive::blocks(current.prims.size());
        size_t start = current.prims.begin();
        Primitive* accel = (Primitive*) alloc->alloc1->malloc(items*sizeof(Primitive),BVH::byteNodeAlignment);
        typename BVH::NodeRef node = bvh->encodeLeaf((char*)accel,items);
        LBBox3fa allBounds = empty;
        for (size_t i=0; i<items; i++)
          allBounds.extend(accel[i].fillMB(prims, start, current.prims.end(), bvh->scene, false, time, bvh->numTimeSteps));
        *current.parent = node;
        return allBounds;
      }

      BVH* bvh;
      PrimRef* prims;
      size_t time;
    };

    template<int N, typename Mesh, typename Primitive>
    struct BVHNBuilderMSMBlurSAH : public Builder
    {
      typedef BVHN<N> BVH;
      typedef typename BVHN<N>::NodeRef NodeRef;
      BVH* bvh;
      Scene* scene;
      mvector<PrimRef> prims; 
      const size_t sahBlockSize;
      const float intCost;
      const size_t minLeafSize;
      const size_t maxLeafSize;

      BVHNBuilderMSMBlurSAH (BVH* bvh, Scene* scene, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize)
        : bvh(bvh), scene(scene), prims(scene->device), 
          sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)) {}

      void build(size_t, size_t) 
      {
	/* skip build for empty scene */
        const size_t numPrimitives = scene->getNumPrimitives<Mesh,true>();

        if (numPrimitives == 0) {
          prims.clear();
          bvh->clear();
          return;
        }      

        double t0 = bvh->preBuild(TOSTRING(isa) "::BVH" + toString(N) + "BuilderMSMBlurSAH");
	
        /* allocate buffers */
        bvh->numTimeSteps = scene->getNumTimeSteps<Mesh,true>();
        const size_t numTimeSegments = bvh->numTimeSteps-1; assert(bvh->numTimeSteps > 1);
        prims.resize(numPrimitives);
        bvh->alloc.init_estimate(numPrimitives*sizeof(PrimRef)*numTimeSegments);
        NodeRef* roots = (NodeRef*) bvh->alloc.threadLocal2()->alloc0->malloc(sizeof(NodeRef)*numTimeSegments,BVH::byteNodeAlignment);

        /* build BVH for each timestep */
        avector<BBox3fa> bounds(bvh->numTimeSteps);
        size_t num_bvh_primitives = 0;
        for (size_t t=0; t<numTimeSegments; t++)
        {
          /* call BVH builder */
          NodeRef root; LBBox3fa tbounds;
          const PrimInfo pinfo = createPrimRefArrayMBlur<Mesh>(t,bvh->numTimeSteps,scene,prims,bvh->scene->progressInterface);
          if (pinfo.size())
          {
            std::tie(root, tbounds) = BVHNBuilderMblur<N>::build(bvh,CreateMSMBlurLeaf<N,Primitive>(bvh,prims.data(),t),bvh->scene->progressInterface,prims.data(),pinfo,
                                                                 sahBlockSize,minLeafSize,maxLeafSize,travCost,intCost);
          }
          else
          {
            tbounds = LBBox3fa(empty);
            root = BVH::emptyNode;
          }
          roots[t] = root;
          bounds[t+0] = tbounds.bounds0;
          bounds[t+1] = tbounds.bounds1;
          num_bvh_primitives = max(num_bvh_primitives,pinfo.size());
        }
        bvh->set(NodeRef((size_t)roots),LBBox3fa(bounds),num_bvh_primitives);
        bvh->msmblur = true;

	/* clear temporary data for static geometry */
	if (scene->isStatic()) 
        {
          prims.clear();
          bvh->shrink();
        }
	bvh->cleanup();
        bvh->postBuild(t0);
      }

      void clear() {
        prims.clear();
      }
    };

    template<int N>
      struct CreateAlignedNodeMB4D
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::AlignedNodeMB4D AlignedNodeMB4D;

      __forceinline CreateAlignedNodeMB4D (BVH* bvh) : bvh(bvh) {}
      
      __forceinline AlignedNodeMB4D* operator() (const isa::BVHBuilderBinnedSAH::BuildRecord& current, BVHBuilderBinnedSAH::BuildRecord* children, const size_t num, FastAllocator::ThreadLocal2* alloc)
      {
        AlignedNodeMB4D* node = (AlignedNodeMB4D*) alloc->alloc0->malloc(sizeof(AlignedNodeMB4D),BVH::byteNodeAlignment); node->clear();
        for (size_t i=0; i<num; i++) {
          children[i].parent = (size_t*)&node->child(i);
        }
        *current.parent = bvh->encodeNode(node);
	return node;
      }

      BVH* bvh;
    };

    template<int N>
      struct CreateAlignedNodeMB2
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::AlignedNodeMB AlignedNodeMB;

      __forceinline CreateAlignedNodeMB2 (BVH* bvh) : bvh(bvh) {}
      
      __forceinline AlignedNodeMB* operator() (const isa::BVHBuilderBinnedSAH::BuildRecord& current, BVHBuilderBinnedSAH::BuildRecord* children, const size_t num, FastAllocator::ThreadLocal2* alloc)
      {
        AlignedNodeMB* node = (AlignedNodeMB*) alloc->alloc0->malloc(sizeof(AlignedNodeMB),BVH::byteNodeAlignment); node->clear();
        for (size_t i=0; i<num; i++) {
          children[i].parent = (size_t*)&node->child(i);
        }
        *current.parent = bvh->encodeNode(node);
	return node;
      }

      BVH* bvh;
    };

    template<int N, typename Mesh, typename Primitive>
    struct BVHNBuilderMSMBlur4DSAH : public Builder
    {
      typedef BVHN<N> BVH;
      typedef typename BVHN<N>::NodeRef NodeRef;
      typedef typename BVHN<N>::AlignedNodeMB AlignedNodeMB;
      typedef typename BVHN<N>::AlignedNodeMB4D AlignedNodeMB4D;
      BVH* bvh;
      Scene* scene;
      mvector<PrimRef> prims; 
      const size_t sahBlockSize;
      const float intCost;
      const size_t minLeafSize;
      const size_t maxLeafSize;
      const size_t mode;
      
      BVHNBuilderMSMBlur4DSAH (BVH* bvh, Scene* scene, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, size_t mode)
        : bvh(bvh), scene(scene), prims(scene->device), 
          sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)), mode(mode) {}
      
      NodeRef recurse(const range<size_t>& dti)
      {
        assert(dti.size() > 0);
        if (dti.size() == 1)
        {
          const BBox1f dt(float(dti.begin())/float(bvh->numTimeSteps-1),
                          float(dti.end  ())/float(bvh->numTimeSteps-1));

          const PrimInfo pinfo = createPrimRefArrayMBlur<Mesh>(dti.begin(),bvh->numTimeSteps,scene,prims,bvh->scene->progressInterface);

          /* reduction function */
          auto reduce = [&] (AlignedNodeMB* node, const LBBox3fa* bounds, const size_t num) -> LBBox3fa
          {
            assert(num <= N);
            LBBox3fa allBounds = empty;
            for (size_t i=0; i<num; i++) {
              node->set(i, bounds[i].global(dt));
              allBounds.extend(bounds[i]);
            }
            return allBounds;
          };
          auto identity = LBBox3fa(empty);
          
          NodeRef root;
          BVHBuilderBinnedSAH::build_reduce<NodeRef>
            (root,typename BVH::CreateAlloc(bvh),identity,CreateAlignedNodeMB2<N>(bvh),reduce,CreateMSMBlurLeaf<N,Primitive>(bvh,prims.data(),dti.begin()),bvh->scene->progressInterface,
             prims.data(),pinfo,N,BVH::maxBuildDepthLeaf,sahBlockSize,minLeafSize,maxLeafSize,travCost,intCost);
          
          return root;
        }
        else
        {
          std::vector<range<size_t>> c;
          c.push_back(dti);
          while (c.size() < N)
          {
            std::pop_heap(c.begin(),c.end());
            auto r = c.back();
            if (r.size() == 1) break;
            c.pop_back();
            auto r2 = r.split();
            c.push_back(std::get<0>(r2)); std::push_heap(c.begin(),c.end());
            c.push_back(std::get<1>(r2)); std::push_heap(c.begin(),c.end());
          }
          
          LBBox3fa lbounds = empty;
          AlignedNodeMB4D* node = (AlignedNodeMB4D*) bvh->alloc.threadLocal2()->alloc0->malloc(sizeof(AlignedNodeMB4D), BVH::byteNodeAlignment); node->clear();
          for (size_t i=0; i<c.size(); i++) 
          {
            NodeRef cnode = recurse(c[i]);

            const BBox1f dt(float(c[i].begin())/float(bvh->numTimeSteps-1),
                            float(c[i].end  ())/float(bvh->numTimeSteps-1));

            avector<PrimRef2> prims2(prims.size()); 
            const PrimInfo2 pinfo = createPrimRef2ArrayMBlur<Mesh>(scene,prims2,bvh->scene->progressInterface,dt);
            LBBox3fa cbounds = pinfo.geomBounds;

            node->set(i,cnode,cbounds,dt);
            lbounds.extend(cbounds);
          }
          NodeRef ref = bvh->encodeNode(node);
          return ref;
        }
      }
      
      void build(size_t, size_t) 
      {
	/* skip build for empty scene */
        const size_t numPrimitives = scene->getNumPrimitives<Mesh,true>();
        if (numPrimitives == 0) { prims.clear(); bvh->clear(); return;  }      
        
        double t0 = bvh->preBuild(TOSTRING(isa) "::BVH" + toString(N) + "BuilderMSMBlur4DSAH");
	
        /* allocate buffers */
        const size_t numTimeSteps = scene->getNumTimeSteps<Mesh,true>();
        const size_t numTimeSegments = numTimeSteps-1; assert(numTimeSteps > 1);
        prims.resize(numPrimitives);
        bvh->alloc.init_estimate(numPrimitives*sizeof(PrimRef)*numTimeSegments);
        bvh->numTimeSteps = numTimeSteps;
        
        NodeRef root = recurse(make_range(size_t(0),numTimeSegments));
        avector<PrimRef2> prims2(numPrimitives); 
        const PrimInfo2 pinfo = createPrimRef2ArrayMBlur<Mesh>(scene,prims2,bvh->scene->progressInterface,BBox1f(0.0f,1.0f));
        bvh->set(root,pinfo.geomBounds,numPrimitives);
        
        /* clear temporary data for static geometry */
        if (scene->isStatic()) 
        {
          prims.clear();
          bvh->shrink();
        }
        bvh->cleanup();
        bvh->postBuild(t0);
      }
        
      void clear() {
        prims.clear();
      }
    };

    /************************************************************************************/ 
    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/

    template<int N, typename Mesh>
      struct CreateAlignedNodeMB4D2
    {
      typedef BVHN<N> BVH;
      typedef HeuristicMBlur<Mesh,NUM_OBJECT_BINS> Heuristic;
      typedef typename Heuristic::Set Set;
      typedef typename Heuristic::Split Split;
      typedef GeneralBuildRecord<Set,Split,PrimInfo2> BuildRecord;
      typedef typename BVH::AlignedNodeMB AlignedNodeMB;
      typedef typename BVH::AlignedNodeMB4D AlignedNodeMB4D;

      __forceinline CreateAlignedNodeMB4D2 (BVH* bvh) : bvh(bvh) {}
      
      __forceinline int operator() (const GeneralBuildRecord<Set,Split,PrimInfo2>& current, GeneralBuildRecord<Set,Split,PrimInfo2>* children, const size_t num, FastAllocator::ThreadLocal2* alloc)
      {
        bool hasTimeSplits = false;
        for (size_t i=0; i<num && !hasTimeSplits; i++)
          hasTimeSplits |= current.pinfo.time_range != children[i].pinfo.time_range;

        if (hasTimeSplits)
        {
          AlignedNodeMB4D* node = (AlignedNodeMB4D*) alloc->alloc0->malloc(sizeof(AlignedNodeMB4D),BVH::byteNodeAlignment); node->clear();
          for (size_t i=0; i<num; i++)
          {
            LBBox3fa cbounds = empty;
            for (size_t j=children[i].prims.object_range.begin(); j<children[i].prims.object_range.end(); j++) 
            {
              PrimRef2& ref = (*children[i].prims.prims)[j];
              cbounds.extend(bvh->scene->getTriangleMesh(ref.geomID())->linearBounds(ref.primID(),children[i].prims.time_range));
            }
            node->set(i,cbounds.global(children[i].prims.time_range),children[i].prims.time_range);
            children[i].parent = (size_t*)&node->child(i);
          }
          *current.parent = bvh->encodeNode(node);
        }
        else
        {
          AlignedNodeMB* node = (AlignedNodeMB*) alloc->alloc0->malloc(sizeof(AlignedNodeMB),BVH::byteNodeAlignment); node->clear();
          for (size_t i=0; i<num; i++)
          {
            LBBox3fa cbounds = empty;
            for (size_t j=children[i].prims.object_range.begin(); j<children[i].prims.object_range.end(); j++) 
            {
              PrimRef2& ref = (*children[i].prims.prims)[j];
              cbounds.extend(bvh->scene->getTriangleMesh(ref.geomID())->linearBounds(ref.primID(),children[i].prims.time_range));
            }
            node->set(i,cbounds.global(children[i].prims.time_range));
            children[i].parent = (size_t*)&node->child(i);
          }
          *current.parent = bvh->encodeNode(node);
        }
	return 0;
      }

      BVH* bvh;
    };

    template<int N, typename Mesh, typename Primitive>
    struct CreateMSMBlurLeaf2
    {
      typedef BVHN<N> BVH;
      typedef HeuristicMBlur<Mesh,NUM_OBJECT_BINS> Heuristic;
      typedef typename Heuristic::Set Set;
      typedef typename Heuristic::Split Split;
      typedef GeneralBuildRecord<Set,Split,PrimInfo2> BuildRecord;

      __forceinline CreateMSMBlurLeaf2 (BVH* bvh) : bvh(bvh) {}
      
      __forceinline const std::pair<LBBox3fa,BBox1f> operator() (const BuildRecord& current, Allocator* alloc)
      {
        size_t items = Primitive::blocks(current.prims.object_range.size());
        size_t start = current.prims.object_range.begin();
        Primitive* accel = (Primitive*) alloc->alloc1->malloc(items*sizeof(Primitive),BVH::byteNodeAlignment);
        typename BVH::NodeRef node = bvh->encodeLeaf((char*)accel,items);
        LBBox3fa allBounds = empty;
        for (size_t i=0; i<items; i++)
          allBounds.extend(accel[i].fillMB(current.prims.prims->data(), start, current.prims.object_range.end(), bvh->scene, current.prims.time_range));
        *current.parent = node;
        return std::make_pair(allBounds.global(current.prims.time_range),current.prims.time_range);
      }

      BVH* bvh;
    };

    template<int N>
    struct CreateMSMBlurLeaf2<N,TriangleMesh,Triangle4vMB>
    {
      typedef BVHN<N> BVH;
      typedef HeuristicMBlur<TriangleMesh,NUM_OBJECT_BINS> Heuristic;
      typedef typename Heuristic::Set Set;
      typedef typename Heuristic::Split Split;
      typedef GeneralBuildRecord<Set,Split,PrimInfo2> BuildRecord;

      __forceinline CreateMSMBlurLeaf2 (BVH* bvh) : bvh(bvh) {}
      
      __forceinline const std::pair<LBBox3fa,BBox1f> operator() (const BuildRecord& current, Allocator* alloc)
      {
        size_t M = Triangle4vMB::fillMBlurBlocks(current.prims.prims->data(), current.prims.object_range, current.prims.time_range, bvh->scene);
        Triangle4vMB* accel = (Triangle4vMB*) alloc->alloc1->malloc(M*sizeof(Triangle4vMB),BVH::byteNodeAlignment);
        Triangle4vMB::fillMBlur(accel, current.prims.prims->data(), current.prims.object_range, current.prims.time_range, bvh->scene);
        *current.parent = bvh->encodeLeaf((char*)accel,M);
        return std::make_pair(LBBox3fa(empty),current.prims.time_range); // returns invalid bounds, we do not use bounds currently
      }

      BVH* bvh;
    };

    template<int N, typename Mesh, typename Primitive>
    struct BVHNBuilderMBlurSAH : public Builder
    {
      typedef BVHN<N> BVH;
      typedef typename BVHN<N>::NodeRef NodeRef;
      typedef typename BVHN<N>::AlignedNodeMB4D AlignedNodeMB4D;

      typedef HeuristicMBlur<Mesh,NUM_OBJECT_BINS> Heuristic;
      typedef typename Heuristic::Set Set;
      typedef typename Heuristic::Split Split;
      typedef GeneralBuildRecord<Set,Split,PrimInfo2> BuildRecord;

      BVH* bvh;
      Scene* scene;
      const size_t sahBlockSize;
      const float intCost;
      const size_t minLeafSize;
      const size_t maxLeafSize;

      BVHNBuilderMBlurSAH (BVH* bvh, Scene* scene, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, const size_t mode)
        : bvh(bvh), scene(scene), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)) {}

      void build(size_t, size_t) 
      {
	/* skip build for empty scene */
        const size_t numPrimitives = scene->getNumPrimitives<Mesh,true>();
        if (numPrimitives == 0) { bvh->clear(); return; }
        
        double t0 = bvh->preBuild(TOSTRING(isa) "::BVH" + toString(N) + "BuilderMBlurSAH");

        /* create primref array */
        std::shared_ptr<avector<PrimRef2>> prims(new avector<PrimRef2>(numPrimitives)); // FIXME: use mvector instead of avector
        PrimInfo2 pinfo = createPrimRef2ArrayMBlur<Mesh>(scene,*prims,bvh->scene->progressInterface);
        
        /* reduction function */
        auto updateNodeFunc = [&] (int node, const std::pair<LBBox3fa,BBox1f>* bounds, const size_t num) -> std::pair<LBBox3fa,BBox1f> {

          assert(num <= N);
          auto allBounds = std::make_pair(LBBox3fa(empty),BBox1f(empty));
          for (size_t i=0; i<num; i++) {
            //node->set(i, bounds[i].first, bounds[i].second);
            allBounds.first .extend(bounds[i].first);
            allBounds.second.extend(bounds[i].second);
          }
          return allBounds;
        };
        auto identity = std::make_pair(LBBox3fa(empty),BBox1f(empty));

        /* call BVH builder */            
        bvh->alloc.init_estimate(pinfo.size()*sizeof(PrimRef));

        /* builder wants log2 of blockSize as input */		  
        const size_t logBlockSize = __bsr(sahBlockSize); 
        assert((sahBlockSize ^ (size_t(1) << logBlockSize)) == 0);

        /* instantiate array binning heuristic */
        Heuristic heuristic(scene);
        auto createAllocFunc = typename BVH::CreateAlloc(bvh);
        auto createNodeFunc = CreateAlignedNodeMB4D2<N,Mesh>(bvh);
        auto createLeafFunc = CreateMSMBlurLeaf2<N,Mesh,Primitive>(bvh);
        auto progressMonitor = bvh->scene->progressInterface;
        
        typedef GeneralBVHMBBuilder<
          BuildRecord,
          Mesh,
          decltype(identity),
          decltype(createAllocFunc()),
          decltype(createAllocFunc),
          decltype(createNodeFunc),
          decltype(updateNodeFunc),
          decltype(createLeafFunc),
          decltype(progressMonitor),
          PrimInfo2> Builder;

        /* instantiate builder */
        Builder builder(scene,
                        identity,
                        createAllocFunc,
                        createNodeFunc,
                        updateNodeFunc,
                        createLeafFunc,
                        progressMonitor,
                        pinfo,
                        N,BVH::maxDepth,logBlockSize,
                        minLeafSize,maxLeafSize,travCost,intCost);
        
        /* build hierarchy */
        Set set(prims); 
        assert(prims->size() == pinfo.size());
        NodeRef root;
        BuildRecord br(pinfo,1,(size_t*)&root,set);
        builder(br);
        
        bvh->set(root,pinfo.geomBounds,pinfo.size());

	/* clear temporary data for static geometry */
	if (scene->isStatic()) bvh->shrink();
	bvh->cleanup();
        bvh->postBuild(t0);
      }

      void clear() {
      }
    };

    /************************************************************************************/ 
    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/

    template<int N, typename Mesh, typename Primitive>
    struct BVHNBuilderMSMBlurTSSAH : public Builder
    {
      typedef BVHN<N> BVH;
      typedef typename BVHN<N>::NodeRef NodeRef;
      typedef typename BVHN<N>::AlignedNodeMB AlignedNodeMB;
      typedef typename BVHN<N>::TimeSplitNode TimeSplitNode;
      BVH* bvh;
      Scene* scene;
      mvector<PrimRef> prims; 
      const size_t sahBlockSize;
      const float intCost;
      const size_t minLeafSize;
      const size_t maxLeafSize;
      const size_t mode;
      
      BVHNBuilderMSMBlurTSSAH (BVH* bvh, Scene* scene, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, size_t mode)
        : bvh(bvh), scene(scene), prims(scene->device), 
          sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)), mode(mode) {}
      
      std::tuple<NodeRef,LBBox3fa> recurse(const range<size_t>& dti)
      {
        assert(dti.size() > 0);
        if (dti.size() == 1)
        {
          const BBox1f dt(float(dti.begin())/float(bvh->numTimeSteps-1),
                          float(dti.end  ())/float(bvh->numTimeSteps-1));

          const PrimInfo pinfo = createPrimRefArrayMBlur<Mesh>(dti.begin(),bvh->numTimeSteps,scene,prims,bvh->scene->progressInterface);

          /* reduction function */
          auto reduce = [&] (AlignedNodeMB* node, const LBBox3fa* bounds, const size_t num) -> LBBox3fa
          {
            assert(num <= N);
            LBBox3fa allBounds = empty;
            for (size_t i=0; i<num; i++) {
              node->set(i, bounds[i].global(dt));
              allBounds.extend(bounds[i]);
            }
            return allBounds;
          };
          auto identity = LBBox3fa(empty);
          
          NodeRef root;
          LBBox3fa root_bounds = BVHBuilderBinnedSAH::build_reduce<NodeRef>
            (root,typename BVH::CreateAlloc(bvh),identity,CreateAlignedNodeMB2<N>(bvh),reduce,CreateMSMBlurLeaf<N,Primitive>(bvh,prims.data(),dti.begin()),bvh->scene->progressInterface,
             prims.data(),pinfo,N,BVH::maxBuildDepthLeaf,sahBlockSize,minLeafSize,maxLeafSize,travCost,intCost);
          
          return std::make_tuple(root,root_bounds.global(dt));
        }
        else
        {
          std::vector<range<size_t>> c;
          c.push_back(dti);
          while (c.size() < N)
          {
            std::pop_heap(c.begin(),c.end());
            auto r = c.back();
            if (r.size() == 1) break;
            c.pop_back();
            auto r2 = r.split();
            c.push_back(std::get<0>(r2)); std::push_heap(c.begin(),c.end());
            c.push_back(std::get<1>(r2)); std::push_heap(c.begin(),c.end());
          }
          
          LBBox3fa lbounds = empty;
          TimeSplitNode* node = (TimeSplitNode*) bvh->alloc.threadLocal2()->alloc0->malloc(sizeof(TimeSplitNode), BVH::byteNodeAlignment); node->clear();
          for (size_t i=0; i<c.size(); i++) 
          {
            NodeRef cnode; LBBox3fa cbounds;
            std::tie(cnode,cbounds) = recurse(c[i]);
            const BBox1f dt(float(c[i].begin())/float(bvh->numTimeSteps-1),
                            float(c[i].end  ())/float(bvh->numTimeSteps-1));
            node->set(i,cnode,dt);
            lbounds.extend(cbounds);
          }
          NodeRef ref = bvh->encodeNode(node);
          return std::make_tuple(ref,lbounds);
        }
      }
      
      void build(size_t, size_t) 
      {
	/* skip build for empty scene */
        const size_t numPrimitives = scene->getNumPrimitives<Mesh,true>();
        if (numPrimitives == 0) { prims.clear(); bvh->clear(); return;  }      
        
        double t0 = bvh->preBuild(TOSTRING(isa) "::BVH" + toString(N) + "BuilderMSMBlurTSSAH");
	
        /* allocate buffers */
        const size_t numTimeSteps = scene->getNumTimeSteps<Mesh,true>();
        const size_t numTimeSegments = numTimeSteps-1; assert(numTimeSteps > 1);
        prims.resize(numPrimitives);
        bvh->alloc.init_estimate(numPrimitives*sizeof(PrimRef)*numTimeSegments);
        bvh->numTimeSteps = numTimeSteps;
        
        NodeRef root; LBBox3fa lbounds;
        std::tie(root, lbounds) = recurse(make_range(size_t(0),numTimeSegments));
        bvh->set(root,lbounds,numPrimitives);
        
        /* clear temporary data for static geometry */
        if (scene->isStatic()) 
        {
          prims.clear();
          bvh->shrink();
        }
        bvh->cleanup();
        bvh->postBuild(t0);
      }
        
      void clear() {
        prims.clear();
      }
    };
      
    /************************************************************************************/ 
    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/

    template<int N>
    __forceinline size_t norotate(typename BVHN<N>::AlignedNode* node, const size_t* counts, const size_t num) {
      return 0;
    }

    template<int N, typename Mesh, typename Primitive, typename Splitter>
    struct BVHNBuilderFastSpatialSAH : public Builder
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::NodeRef NodeRef;
      BVH* bvh;
      Scene* scene;
      Mesh* mesh;
      mvector<PrimRef> prims0;
      const size_t sahBlockSize;
      const float intCost;
      const size_t minLeafSize;
      const size_t maxLeafSize;
      const float splitFactor;

      BVHNBuilderFastSpatialSAH (BVH* bvh, Scene* scene, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, const size_t mode)
        : bvh(bvh), scene(scene), mesh(nullptr), prims0(scene->device), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)),
          splitFactor(scene->device->max_spatial_split_replications) {}

      BVHNBuilderFastSpatialSAH (BVH* bvh, Mesh* mesh, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, const size_t mode)
        : bvh(bvh), scene(nullptr), mesh(mesh), prims0(bvh->device), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)),
          splitFactor(scene->device->max_spatial_split_replications) {}

      // FIXME: shrink bvh->alloc in destructor here and in other builders too

      void build(size_t, size_t) 
      {
	/* skip build for empty scene */
        const size_t numOriginalPrimitives = mesh ? mesh->size() : scene->getNumPrimitives<Mesh,false>();
        if (numOriginalPrimitives == 0) {
          prims0.clear();
          bvh->clear();
          return;
        }

        double t0 = bvh->preBuild(mesh ? "" : TOSTRING(isa) "::BVH" + toString(N) + "BuilderFastSpatialSAH");

        /* create primref array */
        const size_t numSplitPrimitives = max(numOriginalPrimitives,size_t(splitFactor*numOriginalPrimitives));
        prims0.resize(numSplitPrimitives);
        PrimInfo pinfo = mesh ? 
          createPrimRefArray<Mesh>  (mesh ,prims0,bvh->scene->progressInterface) : 
          createPrimRefArray<Mesh,false>(scene,prims0,bvh->scene->progressInterface);

        /* primref array could be smaller due to invalid geometry */
        const size_t numPrimitives = pinfo.size();

        /* calculate total surface area */
        const float A = (float) parallel_reduce(size_t(0),numPrimitives,0.0, [&] (const range<size_t>& r) -> double // FIXME: this sum is not deterministic
                                                {
                                                  double A = 0.0f;
                                                  for (size_t i=r.begin(); i<r.end(); i++)
                                                  {
                                                    PrimRef& prim = prims0[i];
                                                    A += area(prim.bounds());
                                                  }
                                                  return A;
                                                },std::plus<double>());

        const float f = 10.0f;
        const float invA = 1.0f / A;
        /* calculate maximal number of spatial splits per primitive */
        parallel_for( size_t(0), numPrimitives, [&](const range<size_t>& r)
                      {
                        for (size_t i=r.begin(); i<r.end(); i++)
                        {
                          PrimRef& prim = prims0[i];
                          assert((prim.lower.a & 0xFF000000) == 0);
                          const float nf = ceilf(f*pinfo.size()*area(prim.bounds()) * invA);
                          // FIXME: is there a better general heuristic ?
                          size_t n = 4+min(ssize_t(127-4), max(ssize_t(1), ssize_t(nf)));
                          prim.lower.a |= n << 24;              
                        }
                      });
        
        Splitter splitter(scene);


        bvh->alloc.init_estimate(pinfo.size()*sizeof(PrimRef));

        NodeRef root;
        BVHBuilderBinnedFastSpatialSAH::build_reduce<NodeRef>(
          root,
          typename BVH::CreateAlloc(bvh),
          size_t(0),
          typename BVH::CreateAlignedNode(bvh),
          norotate<N>,
          CreateLeafSpatial<N,Primitive>(bvh,prims0.data()),
          splitter,
          bvh->scene->progressInterface,
          prims0.data(),
          numSplitPrimitives,
          pinfo,
          N,BVH::maxBuildDepthLeaf,
          sahBlockSize,minLeafSize,maxLeafSize,
          travCost,intCost);
        

        bvh->set(root,LBBox3fa(pinfo.geomBounds),pinfo.size());      
        bvh->layoutLargeNodes(size_t(pinfo.size()*0.005f));

	/* clear temporary data for static geometry */
	bool staticGeom = mesh ? mesh->isStatic() : scene->isStatic();
	if (staticGeom) {
          prims0.clear();
          bvh->shrink();
        }
	bvh->cleanup();
        bvh->postBuild(t0);
      }

      void clear() {
        prims0.clear();
      }
    };


    /************************************************************************************/ 
    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/

    // FIXME: merge with standard class
    template<int N, typename Primitive>
    struct CreateLeafSweep
    {
      typedef BVHN<N> BVH;

      __forceinline CreateLeafSweep (BVH* bvh, PrimRef* prims) : bvh(bvh), prims(prims) {}
      
      __forceinline size_t operator() (const BVHBuilderSweepSAH::BuildRecord& current, Allocator* alloc)
      {
        size_t n = current.prims.size();
        size_t items = Primitive::blocks(n);
        size_t start = current.prims.begin();
        Primitive* accel = (Primitive*) alloc->alloc1->malloc(items*sizeof(Primitive),BVH::byteNodeAlignment);
        typename BVH::NodeRef node = BVH::encodeLeaf((char*)accel,items);
        for (size_t i=0; i<items; i++) {
          accel[i].fill(prims,start,current.prims.end(),bvh->scene,false);
        }
        *current.parent = node;
	return n;
      }

      BVH* bvh;
      PrimRef* prims;
    };

    template<int N, typename Mesh, typename Primitive>
    struct BVHNBuilderSweepSAH : public Builder
    {
      typedef BVHN<N> BVH;
      BVH* bvh;
      Scene* scene;
      Mesh* mesh;
      mvector<PrimRef> prims;
      const size_t sahBlockSize;
      const float intCost;
      const size_t minLeafSize;
      const size_t maxLeafSize;
      const float presplitFactor;

      BVHNBuilderSweepSAH (BVH* bvh, Scene* scene, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, const size_t mode)
        : bvh(bvh), scene(scene), mesh(nullptr), prims(scene->device), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)),
          presplitFactor((mode & MODE_HIGH_QUALITY) ? defaultPresplitFactor : 1.0f) {}


      BVHNBuilderSweepSAH (BVH* bvh, Mesh* mesh, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize, const size_t mode)
        : bvh(bvh), scene(nullptr), mesh(mesh), prims(bvh->device), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)),
          presplitFactor((mode & MODE_HIGH_QUALITY ) ? defaultPresplitFactor : 1.0f) {}

      // FIXME: shrink bvh->alloc in destructor here and in other builders too

      void build(size_t, size_t) 
      {
	/* skip build for empty scene */
        const size_t numPrimitives = mesh ? mesh->size() : scene->getNumPrimitives<Mesh,false>();
        if (numPrimitives == 0) {
          prims.clear();
          bvh->clear();
          return;
        }
        
        double t0 = bvh->preBuild(mesh ? "" : TOSTRING(isa) "::BVH" + toString(N) + "BuilderSweepSAH");

#if PROFILE
        profile(2,PROFILE_RUNS,numPrimitives,[&] (ProfileTimer& timer) {
#endif

            /* create primref array */
            const size_t numSplitPrimitives = max(numPrimitives,size_t(presplitFactor*numPrimitives));
            prims.resize(numSplitPrimitives);
            PrimInfo pinfo = mesh ? 
              createPrimRefArray<Mesh>  (mesh ,prims,bvh->scene->progressInterface) : 
              createPrimRefArray<Mesh,false>(scene,prims,bvh->scene->progressInterface);
        
            /* perform pre-splitting */
            if (presplitFactor > 1.0f) 
              pinfo = presplit<Mesh>(scene, pinfo, prims);
        
            /* call BVH builder */
            bvh->alloc.init_estimate(pinfo.size()*sizeof(PrimRef));
            BVHNBuilderSweep<N>::build(bvh,CreateLeafSweep<N,Primitive>(bvh,prims.data()),bvh->scene->progressInterface,prims.data(),pinfo,sahBlockSize,minLeafSize,maxLeafSize,travCost,intCost);

#if PROFILE
          }); 
#endif	

	/* clear temporary data for static geometry */
	bool staticGeom = mesh ? mesh->isStatic() : scene->isStatic();
	if (staticGeom) {
          prims.clear();
          bvh->shrink();
        }
	bvh->cleanup();
        bvh->postBuild(t0);
      }

      void clear() {
        prims.clear();
      }
    };

    /************************************************************************************/ 
    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/


#if defined(EMBREE_GEOMETRY_LINES)
    Builder* BVH4Line4iMeshBuilderSAH     (void* bvh, LineSegments* mesh, size_t mode) { return new BVHNBuilderSAH<4,LineSegments,Line4i>((BVH4*)bvh,mesh,4,1.0f,4,inf,mode); }
    Builder* BVH4Line4iSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<4,LineSegments,Line4i>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4Line4iMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMSMBlurSAH<4,LineSegments,Line4i>((BVH4*)bvh,scene ,4,1.0f,4,inf); }
#if defined(__AVX__)
    Builder* BVH8Line4iSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<8,LineSegments,Line4i>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8Line4iMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMSMBlurSAH<8,LineSegments,Line4i>((BVH8*)bvh,scene,4,1.0f,4,inf); }
#endif
#endif

#if defined(EMBREE_GEOMETRY_HAIR)
    Builder* BVH4Bezier1vSceneBuilderSAH   (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<4,BezierCurves,Bezier1v>((BVH4*)bvh,scene,1,1.0f,1,1,mode); }
    Builder* BVH4Bezier1iSceneBuilderSAH   (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<4,BezierCurves,Bezier1i>((BVH4*)bvh,scene,1,1.0f,1,1,mode); }
#endif

#if defined(EMBREE_GEOMETRY_TRIANGLES)
    Builder* BVH4Triangle4MeshBuilderSAH  (void* bvh, TriangleMesh* mesh, size_t mode) { return new BVHNBuilderSAH<4,TriangleMesh,Triangle4>((BVH4*)bvh,mesh,4,1.0f,4,inf,mode); }
    Builder* BVH4Triangle4vMeshBuilderSAH (void* bvh, TriangleMesh* mesh, size_t mode) { return new BVHNBuilderSAH<4,TriangleMesh,Triangle4v>((BVH4*)bvh,mesh,4,1.0f,4,inf,mode); }
    Builder* BVH4Triangle4iMeshBuilderSAH (void* bvh, TriangleMesh* mesh, size_t mode) { return new BVHNBuilderSAH<4,TriangleMesh,Triangle4i>((BVH4*)bvh,mesh,4,1.0f,4,inf,mode); }
    
    Builder* BVH4Triangle4SceneBuilderSAH  (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<4,TriangleMesh,Triangle4>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4Triangle4vSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<4,TriangleMesh,Triangle4v>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4Triangle4iSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<4,TriangleMesh,Triangle4i>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }

    Builder* BVH4Triangle4vMBSceneBuilderSAH (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMSMBlurSAH<4,TriangleMesh,Triangle4vMB>((BVH4*)bvh,scene,4,1.0f,4,inf); }
    Builder* BVH4Triangle4iMBSceneBuilderSAH (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMSMBlurSAH<4,TriangleMesh,Triangle4iMB>((BVH4*)bvh,scene,4,1.0f,4,inf); }
    Builder* BVH4MB4DTriangle4iMBSceneBuilderRootTimeSplits    (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMSMBlur4DSAH<4,TriangleMesh,Triangle4iMB>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4MB4DTriangle4iMBSceneBuilderInternalTimeSplits (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMBlurSAH<4,TriangleMesh,Triangle4iMB>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4MB4DTriangle4vMBSceneBuilderInternalTimeSplits (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMBlurSAH<4,TriangleMesh,Triangle4vMB>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4MBTSTriangle4iMBSceneBuilderSAH (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMSMBlurTSSAH<4,TriangleMesh,Triangle4iMB>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    
    Builder* BVH4Triangle4SceneBuilderFastSpatialSAH  (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderFastSpatialSAH<4,TriangleMesh,Triangle4,TriangleSplitterFactory>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4Triangle4vSceneBuilderFastSpatialSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderFastSpatialSAH<4,TriangleMesh,Triangle4v,TriangleSplitterFactory>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4Triangle4iSceneBuilderFastSpatialSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderFastSpatialSAH<4,TriangleMesh,Triangle4i,TriangleSplitterFactory>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }


    Builder* BVH4QuantizedTriangle4iSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAHQuantized<4,TriangleMesh,Triangle4i>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
#if defined(__AVX__)
    Builder* BVH8Triangle4MeshBuilderSAH  (void* bvh, TriangleMesh* mesh, size_t mode) { return new BVHNBuilderSAH<8,TriangleMesh,Triangle4>((BVH8*)bvh,mesh,4,1.0f,4,inf,mode); }
    Builder* BVH8Triangle4vMeshBuilderSAH (void* bvh, TriangleMesh* mesh, size_t mode) { return new BVHNBuilderSAH<8,TriangleMesh,Triangle4v>((BVH8*)bvh,mesh,4,1.0f,4,inf,mode); }
    Builder* BVH8Triangle4iMeshBuilderSAH (void* bvh, TriangleMesh* mesh, size_t mode) { return new BVHNBuilderSAH<8,TriangleMesh,Triangle4i>((BVH8*)bvh,mesh,4,1.0f,4,inf,mode); }

    Builder* BVH8Triangle4SceneBuilderSAH  (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<8,TriangleMesh,Triangle4>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8Triangle4vSceneBuilderSAH  (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<8,TriangleMesh,Triangle4v>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8Triangle4iSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<8,TriangleMesh,Triangle4i>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8Triangle4vMBSceneBuilderSAH (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMSMBlurSAH<8,TriangleMesh,Triangle4vMB>((BVH8*)bvh,scene,4,1.0f,4,inf); }
    Builder* BVH8Triangle4iMBSceneBuilderSAH (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMSMBlurSAH<8,TriangleMesh,Triangle4iMB>((BVH8*)bvh,scene,4,1.0f,4,inf); }
    Builder* BVH8MB4DTriangle4iMBSceneBuilderRootTimeSplits (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMSMBlur4DSAH<8,TriangleMesh,Triangle4iMB>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8MB4DTriangle4iMBSceneBuilderInternalTimeSplits (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMBlurSAH<8,TriangleMesh,Triangle4iMB>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8MB4DTriangle4vMBSceneBuilderInternalTimeSplits (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMBlurSAH<8,TriangleMesh,Triangle4vMB>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8MBTSTriangle4iMBSceneBuilderSAH (void* bvh, Scene* scene,       size_t mode) { return new BVHNBuilderMSMBlurTSSAH<8,TriangleMesh,Triangle4iMB>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8QuantizedTriangle4iSceneBuilderSAH  (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAHQuantized<8,TriangleMesh,Triangle4i>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8Triangle4SceneBuilderFastSpatialSAH  (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderFastSpatialSAH<8,TriangleMesh,Triangle4,TriangleSplitterFactory>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8Triangle4vSceneBuilderFastSpatialSAH  (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderFastSpatialSAH<8,TriangleMesh,Triangle4v,TriangleSplitterFactory>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }

    /* experimental full sweep builder */
    Builder* BVH8Triangle4SceneBuilderSweepSAH  (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSweepSAH<8,TriangleMesh,Triangle4>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }

#endif
#endif

#if defined(EMBREE_GEOMETRY_QUADS)
    Builder* BVH4Quad4vMeshBuilderSAH     (void* bvh, QuadMesh* mesh, size_t mode)     { return new BVHNBuilderSAH<4,QuadMesh,Quad4v>((BVH4*)bvh,mesh,4,1.0f,4,inf,mode); }
    Builder* BVH4Quad4iMeshBuilderSAH     (void* bvh, QuadMesh* mesh, size_t mode)     { return new BVHNBuilderSAH<4,QuadMesh,Quad4i>((BVH4*)bvh,mesh,4,1.0f,4,inf,mode); }
    Builder* BVH4Quad4vSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<4,QuadMesh,Quad4v>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4Quad4iSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<4,QuadMesh,Quad4i>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4Quad4iMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMSMBlurSAH<4,QuadMesh,Quad4iMB>((BVH4*)bvh,scene ,4,1.0f,4,inf); }
    Builder* BVH4QuantizedQuad4vSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAHQuantized<4,QuadMesh,Quad4v>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4QuantizedQuad4iSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAHQuantized<4,QuadMesh,Quad4i>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH4Quad4vSceneBuilderFastSpatialSAH  (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderFastSpatialSAH<4,QuadMesh,Quad4v,QuadSplitterFactory>((BVH4*)bvh,scene,4,1.0f,4,inf,mode); }

#if defined(__AVX__)
    Builder* BVH8Quad4vSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<8,QuadMesh,Quad4v>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8Quad4iSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAH<8,QuadMesh,Quad4i>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8Quad4iMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMSMBlurSAH<8,QuadMesh,Quad4iMB>((BVH8*)bvh,scene,4,1.0f,4,inf); }
    Builder* BVH8QuantizedQuad4vSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAHQuantized<8,QuadMesh,Quad4v>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8QuantizedQuad4iSceneBuilderSAH     (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderSAHQuantized<8,QuadMesh,Quad4i>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }
    Builder* BVH8Quad4vMeshBuilderSAH     (void* bvh, QuadMesh* mesh, size_t mode)     { return new BVHNBuilderSAH<8,QuadMesh,Quad4v>((BVH8*)bvh,mesh,4,1.0f,4,inf,mode); }
    Builder* BVH8Quad4vSceneBuilderFastSpatialSAH  (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderFastSpatialSAH<8,QuadMesh,Quad4v,QuadSplitterFactory>((BVH8*)bvh,scene,4,1.0f,4,inf,mode); }

#endif
#endif

#if defined(EMBREE_GEOMETRY_USER)

    Builder* BVH4VirtualSceneBuilderSAH    (void* bvh, Scene* scene, size_t mode) {
      int minLeafSize = scene->device->object_accel_min_leaf_size;
      int maxLeafSize = scene->device->object_accel_max_leaf_size;
      return new BVHNBuilderSAH<4,AccelSet,Object>((BVH4*)bvh,scene,4,1.0f,minLeafSize,maxLeafSize,mode);
    }

    Builder* BVH4VirtualMeshBuilderSAH    (void* bvh, AccelSet* mesh, size_t mode) {
      return new BVHNBuilderSAH<4,AccelSet,Object>((BVH4*)bvh,mesh,4,1.0f,1,inf,mode);
    }

    Builder* BVH4VirtualMBSceneBuilderSAH    (void* bvh, Scene* scene, size_t mode) {
      int minLeafSize = scene->device->object_accel_mb_min_leaf_size;
      int maxLeafSize = scene->device->object_accel_mb_max_leaf_size;
      return new BVHNBuilderMSMBlurSAH<4,AccelSet,Object>((BVH4*)bvh,scene,4,1.0f,minLeafSize,maxLeafSize);
    }
#endif
  }
}
