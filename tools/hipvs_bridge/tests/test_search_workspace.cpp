#include "../seekdb_cuvs_bridge.h"
#include <fake_cuvs.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Real bridge, CPU-backed external API boundary. No CAGRA/GPU simulation claims.
static std::map<void*,size_t> allocations;
static std::mutex api_mutex;
static int alloc_calls=0, params_created=0, params_live=0, resources_live=0;
static size_t default_itopk=32;
static int fail_alloc=0, fail_copy=0, fail_params=0, fail_search=0, fail_free=0;
static std::atomic<int> active_searches{0};
#define REQUIRE(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (0)
extern "C" {
const char *cuvsGetLastErrorText(void) { return "host injected error"; }
cuvsError_t cuvsResourcesCreate(cuvsResources_t *r) { *r=++resources_live; return CUVS_SUCCESS; }
cuvsError_t cuvsResourcesDestroy(cuvsResources_t) { --resources_live; return CUVS_SUCCESS; }
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void **p, size_t bytes) {
  std::lock_guard<std::mutex> guard(api_mutex);
  ++alloc_calls;
  if (fail_alloc>0 && --fail_alloc==0) return CUVS_ERROR;
  *p=std::malloc(bytes);
  if (!*p) return CUVS_ERROR;
  allocations[*p]=bytes; return CUVS_SUCCESS;
}
cuvsError_t cuvsRMMFree(cuvsResources_t, void *p, size_t bytes) {
  std::lock_guard<std::mutex> guard(api_mutex);
  REQUIRE(allocations.count(p) && allocations.at(p)==bytes);
  if (fail_free>0 && --fail_free==0) return CUVS_ERROR;
  allocations.erase(p); std::free(p); return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *p) { *p=new std::remove_pointer<cuvsCagraIndexParams_t>::type{}; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t p) { delete p; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *p) {
  if (fail_params>0 && --fail_params==0) return CUVS_ERROR;
  *p=new std::remove_pointer<cuvsCagraSearchParams_t>::type{};
  (*p)->itopk_size=default_itopk;
  ++params_created; ++params_live; return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t p) { --params_live; delete p; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t *p) { *p=new std::remove_pointer<cuvsCagraIndex_t>::type{}; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t p) { delete[] p->base; delete p; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraBuild(cuvsResources_t,cuvsCagraIndexParams_t ip,DLManagedTensor *data,cuvsCagraIndex_t idx) {
  REQUIRE(ip->build_algo==NN_DESCENT && ip->graph_degree==128 && ip->intermediate_graph_degree==256 && ip->nn_descent_niter==100);
  idx->n=data->dl_tensor.shape[0]; idx->dim=data->dl_tensor.shape[1];
  idx->base=new float[idx->n*idx->dim];
  std::memcpy(idx->base,data->dl_tensor.data,idx->n*idx->dim*sizeof(float)); return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearch(cuvsResources_t,cuvsCagraSearchParams_t sp,cuvsCagraIndex_t idx,
                           DLManagedTensor *qt,DLManagedTensor *nt,DLManagedTensor *dt,cuvsFilter filter) {
  if (++active_searches!=1) { --active_searches; return CUVS_ERROR; }
  std::this_thread::sleep_for(std::chrono::microseconds(100));
  if (fail_search>0 && --fail_search==0) { --active_searches; return CUVS_ERROR; }
  REQUIRE(filter.type==NO_FILTER && sp->search_width==4);
  const long nq=qt->dl_tensor.shape[0], k=nt->dl_tensor.shape[1];
  REQUIRE(sp->itopk_size==std::max(default_itopk,std::max(size_t(512),static_cast<size_t>(k)*4)));
  auto *q=static_cast<float*>(qt->dl_tensor.data);
  auto *ids=static_cast<unsigned*>(nt->dl_tensor.data);
  auto *dist=static_cast<float*>(dt->dl_tensor.data);
  for (long row=0;row<nq;++row) {
    std::vector<std::pair<float,unsigned>> ordered;
    for (long i=0;i<idx->n;++i) {
      float d=0; for (long j=0;j<idx->dim;++j) { float delta=q[row*idx->dim+j]-idx->base[i*idx->dim+j]; d+=delta*delta; }
      ordered.emplace_back(d,static_cast<unsigned>(i));
    }
    std::sort(ordered.begin(),ordered.end());
    for (long i=0;i<k;++i) { ids[row*k+i]=ordered[i].second; dist[row*k+i]=ordered[i].first; }
  }
  --active_searches; return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSerialize(cuvsResources_t,const char*,cuvsCagraIndex_t,bool) {
  if (++active_searches!=1) { --active_searches; return CUVS_ERROR; }
  std::this_thread::sleep_for(std::chrono::microseconds(100));
  --active_searches; return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraDeserialize(cuvsResources_t,const char*,cuvsCagraIndex_t idx) {
  idx->n=256; idx->dim=4; idx->base=new float[1024];
  for (int i=0;i<256;++i) for (int j=0;j<4;++j) idx->base[i*4+j]=static_cast<float>(i);
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t idx,int64_t *n) { *n=idx->n; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t idx,int64_t *d) { *d=idx->dim; return CUVS_SUCCESS; }
cudaError_t cudaMemcpy(void *out,const void *in,size_t bytes,cudaMemcpyKind) {
  if (fail_copy>0 && --fail_copy==0) return cudaErrorUnknown;
  REQUIRE(out && in); std::memcpy(out,in,bytes); return cudaSuccess;
}
}

struct Fixture {
  std::vector<float> base; void *handle;
  explicit Fixture(bool deserialize=false):base(1024) {
    for (int i=0;i<256;++i) for (int j=0;j<4;++j) base[i*4+j]=static_cast<float>(i);
    handle=deserialize ? seekdb_cuvs_deserialize("host-fixture",256,4) : seekdb_cuvs_build(base.data(),256,4);
    REQUIRE(handle);
  }
  ~Fixture() { seekdb_cuvs_free(handle); }
  int search(long k=10,long nq=1) {
    std::vector<float> q(nq*4,42), dist(nq*k,-1);
    std::vector<unsigned> ids(nq*k,UINT_MAX);
    int rc=seekdb_cuvs_search(handle,q.data(),nq,k,ids.data(),dist.data());
    if (!rc) for (long row=0;row<nq;++row) { REQUIRE(ids[row*k]==42); REQUIRE(dist[row*k]==0); REQUIRE(ids[row*k+1]==41); REQUIRE(dist[row*k+1]==4); }
    return rc;
  }
};
static void reuse() {
  Fixture f; REQUIRE(f.search()==0);
  int initial_allocs=alloc_calls, initial_params=params_created;
  for (int i=0;i<8;++i) REQUIRE(f.search()==0);
  REQUIRE(alloc_calls==initial_allocs); REQUIRE(params_created==initial_params);
  REQUIRE(f.search(100)==0); int grown=alloc_calls;
  REQUIRE(f.search(10)==0); REQUIRE(f.search(100)==0); REQUIRE(alloc_calls==grown);
  REQUIRE(f.search(100,2)==0); grown=alloc_calls;
  REQUIRE(f.search()==0); REQUIRE(f.search(100,2)==0); REQUIRE(alloc_calls==grown);
  REQUIRE(f.search(200)==0); REQUIRE(f.search(10)==0); // parameters shrink back without changing output
}
static void library_default() {
  default_itopk=1024;
  { Fixture f; REQUIRE(f.search()==0); REQUIRE(f.search(200)==0); REQUIRE(f.search()==0); }
  default_itopk=32;
}
static void failure_recovery() {
  Fixture f; REQUIRE(f.search()==0);
  for (int point=1;point<=3;++point) {
    fail_alloc=point; REQUIRE(f.search(100,point+2)!=0); fail_alloc=0;
    REQUIRE(f.search()==0);
  }
  for (int point=1;point<=3;++point) { fail_copy=point; REQUIRE(f.search()!=0); fail_copy=0; REQUIRE(f.search()==0); }
  fail_search=1; REQUIRE(f.search()!=0); REQUIRE(f.search()==0);
}
static void free_failure() {
  Fixture f; REQUIRE(f.search()==0);
  fail_free=1; REQUIRE(f.search(100)!=0); fail_free=0;
  REQUIRE(f.search()==0); REQUIRE(f.search(100)==0);
}
static void parameter_failure() {
  Fixture f; fail_params=1; REQUIRE(f.search()!=0); REQUIRE(f.search()==0);
}
static void invalid_inputs() {
  Fixture f; float q[4]={}; unsigned ids[10]={}; int before=alloc_calls;
  REQUIRE(seekdb_cuvs_search(f.handle,q,0,10,ids,nullptr)!=0);
  REQUIRE(seekdb_cuvs_search(f.handle,q,-1,10,ids,nullptr)!=0);
  REQUIRE(seekdb_cuvs_search(f.handle,q,1,0,ids,nullptr)!=0);
  REQUIRE(seekdb_cuvs_search(f.handle,q,1,257,ids,nullptr)!=0);
  if (SIZE_MAX / sizeof(float) / 4 < static_cast<size_t>(LONG_MAX)) {
    REQUIRE(seekdb_cuvs_search(f.handle,q,LONG_MAX,10,ids,nullptr)!=0);
  } else {
    std::puts("SKIP overflow rejection: host long is too small for this size_t overflow fixture; run on LP64 Linux");
  }
  REQUIRE(seekdb_cuvs_search(f.handle,nullptr,1,10,ids,nullptr)!=0);
  REQUIRE(seekdb_cuvs_search(f.handle,q,1,10,nullptr,nullptr)!=0);
  REQUIRE(alloc_calls==before);
  REQUIRE(seekdb_cuvs_search(f.handle,q,1,10,ids,nullptr)==0);
}
static void concurrent_search() {
  Fixture f; std::atomic<int> errors{0}; std::vector<std::thread> threads;
  for (int i=0;i<4;++i) threads.emplace_back([&] { for (int j=0;j<20;++j) if (f.search((j%2)?100:10)) ++errors; });
  for (auto &t:threads) { t.join(); }
  REQUIRE(errors==0);
}
static void concurrent_serialization() {
  Fixture f; std::atomic<int> errors{0}; std::atomic<bool> start{false};
  std::thread search([&] { while (!start.load()) { std::this_thread::yield(); } for (int i=0;i<40;++i) if (f.search()) ++errors; });
  std::thread serialize([&] { while (!start.load()) { std::this_thread::yield(); } for (int i=0;i<40;++i) if (seekdb_cuvs_serialize(f.handle,"host-fixture")) ++errors; });
  start=true; search.join(); serialize.join(); REQUIRE(errors==0);
}
static void restored_workspace() {
  Fixture f(true); REQUIRE(f.search()==0); int count=alloc_calls;
  REQUIRE(f.search()==0); REQUIRE(alloc_calls==count);
  REQUIRE(seekdb_cuvs_serialize(f.handle,"host-fixture")==0);
}
int main(int argc,char **argv) {
  const std::pair<const char*,void(*)()> tests[]={{"reuse",reuse},{"library_default",library_default},{"failure_recovery",failure_recovery},{"free_failure",free_failure},{"parameter_failure",parameter_failure},{"invalid_inputs",invalid_inputs},{"concurrent_search",concurrent_search},{"concurrent_serialization",concurrent_serialization},{"restored_workspace",restored_workspace}};
  int failed=0;
  for (auto test:tests) {
    if (argc>1 && std::strcmp(argv[1],test.first)!=0) continue;
    try { test.second(); REQUIRE(allocations.empty()); REQUIRE(params_live==0); REQUIRE(resources_live==0); std::printf("PASS %s\n",test.first); }
    catch (const std::exception &e) { ++failed; std::printf("FAIL %s: %s\n",test.first,e.what()); }
  }
  return failed?1:0;
}
