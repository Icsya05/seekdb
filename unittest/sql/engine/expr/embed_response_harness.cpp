#include <map>
#include <string>
#include <vector>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT -1
#define OB_INVALID_DATA -2
#define OB_ISNULL(x) ((x)==nullptr)
#define OB_SUCC(x) ((x)==0)
#define OB_FAIL(x) ((ret=(x))!=0)
namespace common {
enum class ObJsonNodeType {J_OBJECT,J_ARRAY,J_STRING,J_INT,J_UINT,J_DOUBLE,J_DECIMAL};
struct ObIAllocator {};
struct ObIJsonBase {
 virtual ~ObIJsonBase()=default;
 virtual ObJsonNodeType json_type() const=0;
 virtual int64_t get_int() const{return 0;}
 virtual uint64_t get_uint() const{return 0;}
 virtual int to_double(double&) const{return -1;}
};
using ObJsonNode=ObIJsonBase;
struct ObJsonObject:ObIJsonBase {
 std::map<std::string,ObJsonNode*> values;
 ObJsonNodeType json_type() const override{return ObJsonNodeType::J_OBJECT;}
 ObJsonNode*get_value(const char*k){auto i=values.find(k);return i==values.end()?nullptr:i->second;}
};
struct ObJsonArray:ObIJsonBase {
 std::vector<ObJsonNode*> values;
 ObJsonNodeType json_type() const override{return ObJsonNodeType::J_ARRAY;}
 long element_count()const{return values.size();}
 ObJsonNode*get_value(long i)const{return values.at(i);}
 int append(ObJsonNode*n){values.push_back(n);return 0;}
};
struct Number:ObIJsonBase {
 double value; ObJsonNodeType type;
 Number(double v,ObJsonNodeType t=ObJsonNodeType::J_DOUBLE):value(v),type(t){}
 ObJsonNodeType json_type()const override{return type;}
 int64_t get_int()const override{return static_cast<int64_t>(value);}
 uint64_t get_uint()const override{return static_cast<uint64_t>(value);}
 int to_double(double&out)const override{out=value;return 0;}
};
struct String:ObIJsonBase {ObJsonNodeType json_type()const override{return ObJsonNodeType::J_STRING;}};
template<class T> struct ObArray {
 std::vector<T> values;
 int push_back(T x){values.push_back(x);return 0;}
 T& at(long i){return values.at(i);}
 long count()const{return values.size();}
};
}
using namespace common;
struct ObAIFuncJsonUtils {
 static int get_json_array(ObIAllocator&,ObJsonArray*&out){out=new ObJsonArray;return 0;}
};
struct ObOpenAIUtils {struct ObOpenAIEmbed {
 static int parse_output(ObIAllocator&,ObJsonObject*,ObIJsonBase*&);
};};
/* ACTUAL_PARSER */
int main(int argc,char**argv){
 assert(argc==2);std::string mode=argv[1];
 ObIAllocator a;ObJsonObject response,first,second;ObJsonArray data,v0,v1;
 Number n0(0),n1(1),i0(0,ObJsonNodeType::J_INT),i1(1,ObJsonNodeType::J_INT);
 v0.values={&n0};v1.values={&n1};data.values={&first,&second};
 first.values["embedding"]=&v0;second.values["embedding"]=&v1;response.values["data"]=&data;
 ObIJsonBase*out=nullptr;int expected=OB_INVALID_DATA;
 Number bad(-1,ObJsonNodeType::J_INT),fraction(0.5),infinity(std::numeric_limits<double>::infinity());String str;
 if(mode=="containers"){
  assert(ObOpenAIUtils::ObOpenAIEmbed::parse_output(a,&response,out)==0);delete out;out=nullptr;
  response.values["data"]=&first;
 }else if(mode=="order"){
  first.values["index"]=&i1;second.values["index"]=&i0;expected=0;
 }else if(mode=="duplicate"){
  first.values["index"]=&i0;second.values["index"]=&i0;
 }else if(mode=="mixed"){first.values["index"]=&i0;
 }else if(mode=="negative"||mode=="range"||mode=="fractional"){
  if(mode=="range")bad.value=2;
  first.values["index"]=mode=="fractional"?&fraction:&bad;second.values["index"]=&i1;
 }else if(mode=="string"){v0.values={&str};
 }else if(mode=="empty"){v0.values.clear();
 }else if(mode=="infinity"){v0.values={&infinity};}
 assert(ObOpenAIUtils::ObOpenAIEmbed::parse_output(a,&response,out)==expected);
 if(expected==0){assert(static_cast<ObJsonArray*>(out)->get_value(0)==&v1);delete out;}
}
