#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT -1
#define OB_ERR_UNEXPECTED -2
#define OB_SUCC(x) ((x)==0)
#define OB_FAIL(x) ((ret=(x))!=0)
#define OB_NOT_NULL(x) ((x)!=nullptr)
#define OB_ISNULL(x) ((x)==nullptr)
#define LOG_USER_ERROR(...)
#define N_AI_EMBED 0
struct ObString {
 std::string value;
 ObString()=default; ObString(const char*s):value(s){} ObString(std::string s):value(s){}
 bool empty()const{return value.empty();} int64_t length()const{return value.size();}
 bool operator==(const ObString&o)const{return value==o.value;}
};
struct ObIAllocator {};
struct ObDatum {
 bool null=false; ObString text; int64_t integer=0;
 bool is_null()const{return null;} void set_null(){null=true;}
 ObString get_string()const{assert(!null);return text;}
 int64_t get_int()const{assert(!null);return integer;}
};
struct ObBitVector {
 bool bits[16]{}; bool at(int64_t i)const{return bits[i];} void set(int64_t i){bits[i]=true;}
};
struct ObDatumVector {
 ObDatum*data=nullptr; bool batch=true;
 ObDatum*at(int64_t i)const{return data+(batch?i:0);}
};
struct ObEvalCtx {
 int exec_ctx_=0;
 struct TempAllocGuard {ObIAllocator alloc;TempAllocGuard(ObEvalCtx&){} ObIAllocator&get_allocator(){return alloc;}};
};
struct MultimodeAlloctor:ObIAllocator {explicit MultimodeAlloctor(ObIAllocator&) {}};
namespace lib {struct ObMemAttr {explicit ObMemAttr(int){}};struct ObMallocHookAttrGuard{explicit ObMallocHookAttrGuard(ObMemAttr){}};}
template<class T>struct ObArray {
 std::vector<T> data;int push_back(const T&v){data.push_back(v);return 0;}
 T&at(int64_t i){return data.at(i);} const T&at(int64_t i)const{return data.at(i);}
 int64_t count()const{return data.size();}
};
struct ObExpr {
 int arg_cnt_=2;ObExpr*args_[3]{};mutable ObBitVector flags;mutable ObDatum data[16];bool batch=true;int fail=0;
 struct Meta {int type_=0; bool lob=false;bool has_lob_header()const{return lob;}} datum_meta_,obj_meta_;
 ObDatumVector locate_expr_datumvector(ObEvalCtx&)const{return {data,batch};}
 ObBitVector&get_evaluated_flags(ObEvalCtx&)const{return flags;}
 int eval_batch(ObEvalCtx&,const ObBitVector&,int64_t)const{return fail;}
};
static int reads=0;
struct ObTextStringHelper {
 static int read_real_string_data(int,ObIAllocator&,const ObDatum&d,const ObExpr::Meta&,bool lob,ObString&out){
  ++reads;out=d.get_string();if(lob){assert(out.value.rfind("LOB:",0)==0);out.value=out.value.substr(4);}return 0;
 }
};
struct ObTextStringDatumResult {
 ObDatum*out;int64_t row=-1;const ObExpr*expr;
 ObTextStringDatumResult(int,const ObExpr*e,ObEvalCtx*,ObDatum*d):out(d),expr(e){}
 int init_with_batch_idx(int64_t,int64_t i){row=i;assert(out==&expr->data[i]);return 0;}
 int append(const ObString&s){assert(row>=0);out->text=s;return 0;}
 void set_result(){out->null=false;}
};
struct ObJsonInt {int64_t value=0;};
struct ObJsonObject {int64_t dimension=0;int add(const char*,ObJsonInt*d){dimension=d->value;return 0;}};
struct ObAIFuncExprInfo {ObString model;};
namespace oceanbase {
namespace share {struct ObAiModelEndpointInfo{};template<class T>T*server_service(){static T service;return &service;}}
namespace query {struct ObIAiEndpointResolver{int resolve_by_model_name(ObString&,ObIAllocator&,share::ObAiModelEndpointInfo&){return 0;}};}
}
namespace share=oceanbase::share;namespace query=oceanbase::query;
struct Request {std::string model;int64_t dim;std::vector<std::string>inputs;};
static std::vector<Request>requests;static bool provider_failure=false,count_mismatch=false;
struct ObAIFuncUtils {
 static int get_ai_func_info(ObIAllocator&,const ObString&m,ObAIFuncExprInfo*&out){static ObAIFuncExprInfo info;info.model=m;out=&info;return 0;}
};
struct ObAIFuncJsonUtils {
 static int get_json_object(ObIAllocator&,ObJsonObject*&out){static ObJsonObject object;object.dimension=0;out=&object;return 0;}
 static int get_json_int(ObIAllocator&,int64_t v,ObJsonInt*&out){static ObJsonInt number;number.value=v;out=&number;return 0;}
};
struct ObAIFuncModel {
 ObString model;ObAIFuncModel(ObIAllocator&,ObAIFuncExprInfo&i,const share::ObAiModelEndpointInfo&):model(i.model){}
 int call_dense_embedding_vector_v2(ObArray<ObString>&content,ObJsonObject*config,ObArray<ObString>&out){
  Request req{model.value,config?config->dimension:0,{}};
  for(auto&s:content.data){req.inputs.push_back(s.value);out.push_back(ObString("["+s.value+"]"));}
  requests.push_back(req);if(provider_failure)return -77;if(count_mismatch)out.data.pop_back();return 0;
 }
};
struct ObExprAIEmbed {
 static constexpr int MODEL_IDX=0,CONTENT_IDX=1,DIM_IDX=2;
 static int eval_ai_embed_batch(const ObExpr&,ObEvalCtx&,const ObBitVector&,int64_t);
};
/* ACTUAL_BATCH */
int main(int argc,char**argv){
 assert(argc==2);std::string mode=argv[1];ObExpr expr,model,content,dim;ObEvalCtx ctx;ObBitVector skip;
 expr.args_[0]=&model;expr.args_[1]=&content;expr.args_[2]=&dim;model.batch=false;model.data[0].text="model-A";
 for(int i=0;i<4;++i){content.data[i].text=ObString(std::to_string(i+1));dim.data[i].integer=2;expr.data[i].text="sentinel";}
 if(mode=="null")content.data[1].null=true;
 else if(mode=="all_null")for(auto&d:content.data)d.null=true;
 else if(mode=="skip"){skip.set(1);expr.flags.set(2);content.data[1].text="";}
 else if(mode=="scalar_content")content.batch=false;
 else if(mode=="groups"){
  expr.arg_cnt_=3;model.batch=true;
  model.data[0].text="model-A";model.data[1].text="model-B";model.data[2].text="model-A";model.data[3].text="model-A";dim.data[2].integer=3;
 }else if(mode=="invalid_dim"){expr.arg_cnt_=3;dim.data[3].integer=0;}
 else if(mode=="null_dim"){expr.arg_cnt_=3;dim.data[3].null=true;}
 else if(mode=="null_model")model.data[0].null=true;
 else if(mode=="empty")content.data[3].text="";
 else if(mode=="arg_failure")content.fail=-66;
 else if(mode=="failure")provider_failure=true;
 else if(mode=="count")count_mismatch=true;
 else if(mode=="lob"){content.obj_meta_.lob=true;for(int i=0;i<4;++i)content.data[i].text=ObString("LOB:"+std::to_string(i+1));}
 int ret=ObExprAIEmbed::eval_ai_embed_batch(expr,ctx,skip,4);
 if(mode=="invalid_dim"||mode=="null_dim"||mode=="null_model"||mode=="empty")assert(ret==OB_INVALID_ARGUMENT&&requests.empty());
 else if(mode=="arg_failure")assert(ret==-66&&requests.empty());
 else if(mode=="failure"||mode=="count"){
  assert(ret==(mode=="failure"?-77:OB_ERR_UNEXPECTED));for(int i=0;i<4;++i)assert(!expr.flags.at(i));
 }else{
  assert(ret==0);
  if(mode=="all_null"){assert(requests.empty());for(int i=0;i<4;++i)assert(expr.flags.at(i)&&expr.data[i].null);return 0;}
  if(mode=="groups"){
   assert(requests.size()==3);assert(requests[0].model=="model-A"&&requests[0].dim==2&&requests[0].inputs==std::vector<std::string>({"1","4"}));
   assert(requests[1].model=="model-B"&&requests[1].dim==2);assert(requests[2].model=="model-A"&&requests[2].dim==3);
  }else assert(requests.size()==1);
  for(int i=0;i<4;++i){
   if(mode=="skip"&&(i==1||i==2)){assert(expr.data[i].text.value=="sentinel");assert(expr.flags.at(i)==(i==2));}
   else if(mode=="null"&&i==1)assert(expr.data[i].null&&expr.flags.at(i));
   else assert(expr.flags.at(i)&&expr.data[i].text.value=="["+std::to_string(mode=="scalar_content"?1:i+1)+"]");
  }
  if(mode=="null")assert(requests[0].inputs==std::vector<std::string>({"1","3","4"}));
  else if(mode=="skip")assert(requests[0].inputs==std::vector<std::string>({"1","4"}));
  else if(mode=="batch")assert(requests[0].inputs==std::vector<std::string>({"1","2","3","4"}));
  if(mode=="repeat"){
   expr.flags=ObBitVector();for(int i=0;i<4;++i)content.data[i].text=ObString(std::to_string(i+5));
   assert(ObExprAIEmbed::eval_ai_embed_batch(expr,ctx,skip,2)==0);assert(requests.size()==2&&requests[1].inputs==std::vector<std::string>({"5","6"}));
   assert(expr.data[0].text.value=="[5]"&&expr.data[1].text.value=="[6]"&&expr.data[2].text.value=="[3]");
  }
 }
}
