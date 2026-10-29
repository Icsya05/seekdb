#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>
#define OB_SUCCESS 0
#define OB_ERR_UNEXPECTED -1
#define OB_INVALID_DATA -2
#define OB_INVALID_ARGUMENT -3
#define INIT_SUCC(x) int x=0
#define OB_ISNULL(x) ((x)==nullptr)
#define OB_NOT_NULL(x) ((x)!=nullptr)
#define OB_SUCC(x) ((x)==0)
#define OB_FAIL(x) ((ret=(x))!=0)
#define LOG_USER_ERROR(...)
using ObString=std::string;
struct ObIAllocator {};
enum class ObJsonNodeType {J_OBJECT,J_ARRAY,J_INT,J_UINT,J_DOUBLE,J_DECIMAL,J_STRING};
struct ObJsonNode {
  virtual ~ObJsonNode()=default;
  virtual ObJsonNodeType json_type()const=0;
  virtual int64_t get_int()const{return 0;}
  virtual uint64_t get_uint()const{return 0;}
  virtual int to_double(double&)const{return -1;}
  virtual ObJsonNode*clone(ObIAllocator*,bool)const{return nullptr;}
};
struct ObJsonInt:ObJsonNode {
  int64_t n; explicit ObJsonInt(int64_t v):n(v){}
  ObJsonNodeType json_type()const override{return ObJsonNodeType::J_INT;}
  int64_t value()const{return n;}
  int64_t get_int()const override{return n;}
  void set_value(int64_t v){n=v;}
  int to_double(double&v)const override{v=n;return 0;}
};
struct ObJsonDouble:ObJsonNode {
  double n; explicit ObJsonDouble(double v):n(v){}
  ObJsonNodeType json_type()const override{return ObJsonNodeType::J_DOUBLE;}
  double value()const{return n;}
  int to_double(double&v)const override{v=n;return 0;}
};
struct String:ObJsonNode {ObJsonNodeType json_type()const override{return ObJsonNodeType::J_STRING;}};
struct ObJsonObject:ObJsonNode {
  std::map<ObString,ObJsonNode*> values;
  ObJsonNodeType json_type()const override{return ObJsonNodeType::J_OBJECT;}
  ObJsonNode*get_value(const ObString&k){auto i=values.find(k);return i==values.end()?nullptr:i->second;}
  int add(const ObString&k,ObJsonNode*v){values[k]=v;return 0;}
  ObJsonNode*clone(ObIAllocator*,bool)const override{return new ObJsonObject(*this);}
};
struct ObJsonArray:ObJsonNode {
  std::vector<ObJsonNode*> values;
  ObJsonNodeType json_type()const override{return ObJsonNodeType::J_ARRAY;}
  int64_t element_count()const{return values.size();}
  ObJsonNode*get_value(int64_t i)const{return values.at(i);}
  int append(ObJsonNode*n){values.push_back(n);return 0;}
};
template<class T>struct ObArray {
  std::vector<T> values;
  int push_back(T x){values.push_back(x);return 0;}
  decltype(auto) at(int64_t i){return values.at(i);}
  int64_t count()const{return values.size();}
  auto begin(){return values.begin();} auto end(){return values.end();}
};
struct ObAIFuncJsonUtils {
  static int get_json_array(ObIAllocator&,ObJsonArray*&out){out=new ObJsonArray;return 0;}
  static int get_json_int(ObIAllocator&,int64_t n,ObJsonInt*&out){out=new ObJsonInt(n);return 0;}
};
using ObIJsonBase=ObJsonNode;
namespace common {
using ::ObIAllocator;using ::ObJsonObject;using ::ObJsonArray;using ::ObIJsonBase;
}
struct ObDashscopeUtils {struct ObDashscopeRerank {
  static int parse_output(ObIAllocator&,ObJsonObject*,ObIJsonBase*&);
};};
struct ObSiliconflowUtils {struct ObSiliconflowRerank {
  static int parse_output(ObIAllocator&,ObJsonObject*,ObIJsonBase*&);
};};
struct ObExprAIRerank {
  static constexpr const char*INDEX_KEY="index";
  static constexpr const char*SCORE_KEY="relevance_score";
  static constexpr const char*MODEL_SCORE_KEY="_model_score";
  static int get_doc_array_from_documents_array_with_key(ObIAllocator&,ObJsonArray*,ObString&,ObJsonArray*&);
  static int sort_document_array_by_model_result(ObIAllocator&,ObJsonArray*,ObJsonArray*,ObJsonArray*&);
  static int batch_result_add_base(ObIAllocator&,ObJsonArray*,int64_t);
  static int compact_json_array_by_key(ObIAllocator&,ObJsonArray*,ObJsonArray*,ObString&,ObJsonArray*&);
};
/* PRODUCTION_FUNCTIONS */
int main(int argc,char**argv){
  assert(argc==2);ObString mode=argv[1],key="relevance_score";ObIAllocator a;
  ObJsonInt i0(0),i1(1),i2(2),i3(3),integer(9),bad(-1);
  ObJsonDouble s0(10),s1(12),s2(11),s3(9),fraction(0.5),inf(std::numeric_limits<double>::infinity());
  ObJsonObject r0,r1,r2,r3;String str;
  r0.values={{"index",&i0},{key,&s0}};r1.values={{"index",&i1},{key,&s1}};
  r2.values={{"index",&i2},{key,&s2}};r3.values={{"index",&i3},{key,&s3}};
  ObJsonArray left,right;left.values={&r0};right.values={&r1,&r2,&r3};ObJsonArray*out=nullptr;
  if(mode.find("siliconflow_")==0||mode.find("dashscope_")==0){
    ObJsonObject response,output;ObIJsonBase*parsed=nullptr;
    response.values["results"]=&right;output.values["results"]=&right;response.values["output"]=&output;
    if(mode=="siliconflow_container")response.values["results"]=&str;
    if(mode=="dashscope_output")response.values["output"]=&str;
    if(mode=="dashscope_container")output.values["results"]=&str;
    int ret=mode.find("siliconflow_")==0?
        ObSiliconflowUtils::ObSiliconflowRerank::parse_output(a,&response,parsed):
        ObDashscopeUtils::ObDashscopeRerank::parse_output(a,&response,parsed);
    bool valid=mode=="siliconflow_valid"||mode=="dashscope_valid";
    assert(ret==(valid?0:OB_INVALID_DATA));if(valid)assert(parsed==&right);return 0;
  }
  if(mode=="doc_key"||mode=="missing_doc_key"){
    ObString text_key="text";r0.values["text"]=&str;
    if(mode=="missing_doc_key")text_key="missing";
    int ret=ObExprAIRerank::get_doc_array_from_documents_array_with_key(a,&left,text_key,out);
    assert(ret==(mode=="doc_key"?0:OB_INVALID_ARGUMENT));
    if(ret==0){assert(out->values==std::vector<ObJsonNode*>{&str});delete out;}return 0;
  }
  if(mode=="object_integer_score"||mode=="object_range"){
    ObJsonObject doc;doc.values={{"text",&str},{"_model_score",&s0}};ObJsonArray docs,results;
    docs.values={&doc};results.values={&r0};r0.values[key]=&integer;
    if(mode=="object_range")i0.n=1;
    int ret=ObExprAIRerank::sort_document_array_by_model_result(a,&docs,&results,out);
    assert(ret==(mode=="object_range"?OB_INVALID_DATA:0));
    if(ret==0){auto*copy=static_cast<ObJsonObject*>(out->get_value(0));
      assert(copy!=&doc&&copy->get_value("text")==&str&&copy->get_value("_model_score")==&integer);
      assert(doc.get_value("_model_score")==&s0);delete copy;delete out;}return 0;
  }
  if(mode=="unequal"||mode=="ties"||mode=="unsorted"||mode=="integer_score"){
    if(mode=="ties"){s1.n=s2.n=s3.n=10;}
    if(mode=="unsorted"){right.values={&r3,&r1,&r2};}
    if(mode=="integer_score"){r3.values[key]=&integer;}
    assert(ObExprAIRerank::compact_json_array_by_key(a,&left,&right,key,out)==0);
    std::vector<ObJsonNode*> want=mode=="ties"?std::vector<ObJsonNode*>{&r0,&r1,&r2,&r3}:
                                                        std::vector<ObJsonNode*>{&r1,&r2,&r0,&r3};
    assert(out->values==want);delete out;return 0;
  }
  right.values={&r0,&r1};
  if(mode=="offset"){
    assert(ObExprAIRerank::batch_result_add_base(a,&right,20)==0);
    assert(r0.get_value("index")->get_int()==20&&r1.get_value("index")->get_int()==21);return 0;
  }
  if(mode=="range"){i0.n=2;}
  if(mode=="negative"){r0.values["index"]=&bad;}
  if(mode=="duplicate"){r1.values["index"]=&i0;}
  if(mode=="fractional_index"){r0.values["index"]=&fraction;}
  if(mode=="string_score"){r0.values[key]=&str;}
  if(mode=="infinity"){r0.values[key]=&inf;}
  if(mode=="object_type"){right.values[0]=&str;}
  assert(ObExprAIRerank::batch_result_add_base(a,&right,20)!=0);
}
