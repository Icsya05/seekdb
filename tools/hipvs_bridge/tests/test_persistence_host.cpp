// CPU API boundary test for smoke control flow; does not test GPU persistence.
#define cuvsCagraSerialize host_success_serialize
#define main existing_host_contract_main
#include "test_search_workspace.cpp"
#undef main
#undef cuvsCagraSerialize
static bool inject_serialize_failure=false;
extern "C" cuvsError_t cuvsCagraSerialize(cuvsResources_t res,const char *name,cuvsCagraIndex_t idx,bool data) {
  if (inject_serialize_failure) return CUVS_ERROR;
  return host_success_serialize(res,name,idx,data);
}
#define main persistence_smoke_main
#include "../test_persistence.cpp"
#undef main
int main(int argc,char **argv) {
  if (argc==4) {
    if (std::strcmp(argv[3],"serialize-failure")==0) inject_serialize_failure=true;
    else if (std::strcmp(argv[3],"copy-failure")==0) fail_copy=3;
    else return 2;
  }
  int rc=persistence_smoke_main(argc==4?3:argc,argv);
  if (!allocations.empty() || resources_live!=0 || params_live!=0) return 99;
  return rc;
}
