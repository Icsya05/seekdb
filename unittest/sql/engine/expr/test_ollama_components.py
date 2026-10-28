"""Run production Ollama methods with JSON/allocation boundary doubles.

These focused component tests complement test_ollama_provider.py; they do not
replace a full-server or production-header build.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]

SCAFFOLD = r'''
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <strings.h>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT -1
#define OB_INVALID_DATA -2
#define OB_ERR_UNEXPECTED -3
#define OB_ALLOCATE_MEMORY_FAILED -4
#define OB_NOT_SUPPORTED -5
#define OB_AI_FUNC_PARAM_EMPTY -6
#define OB_AI_FUNC_PARAM_VALUE_INVALID -7
#define OB_ISNULL(x) ((x)==nullptr)
#define OB_SUCC(x) ((x)==0)
#define OB_FAIL(x) ((ret=(x))!=0)
#define OB_NEWx(type, allocator, ...) new type(__VA_ARGS__)
#define LOG_USER_ERROR(...)
#define LOG_WARN(...)
#define ARRAYSIZEOF(x) (sizeof(x)/sizeof(x[0]))
namespace common {
struct ObIAllocator {};
struct ObString {
 std::string value;
 ObString()=default; ObString(const char*s):value(s){}
 bool empty()const{return value.empty();}
 int case_compare(const char*s)const{return strcasecmp(value.c_str(),s);}
 int case_compare(const ObString&s)const{return case_compare(s.value.c_str());}
};
template<class T>struct ObArray:std::vector<T>{int push_back(T v){std::vector<T>::push_back(v);return 0;}};
enum class ObJsonNodeType {J_OBJECT,J_ARRAY,J_STRING,J_BOOLEAN,J_INT};
struct ObIJsonBase {
 virtual ~ObIJsonBase()=default;
 virtual ObJsonNodeType json_type()const=0;
 virtual long get_data_length()const{return 0;}
 virtual const char*get_data()const{return nullptr;}
 virtual int seek(const struct ObJsonPath&,long,bool,bool,struct ObJsonSeekResult&){return OB_INVALID_DATA;}
};
using ObJsonNode=ObIJsonBase;
struct ObJsonString:ObIJsonBase {
 std::string value; explicit ObJsonString(const ObString&s):value(s.value){}
 ObJsonNodeType json_type()const override{return ObJsonNodeType::J_STRING;}
 long get_data_length()const override{return value.size();}
 const char*get_data()const override{return value.c_str();}
};
struct ObJsonBoolean:ObIJsonBase {
 bool value; explicit ObJsonBoolean(bool b):value(b){}
 ObJsonNodeType json_type()const override{return ObJsonNodeType::J_BOOLEAN;}
};
struct ObJsonObject:ObIJsonBase {
 std::map<std::string,ObJsonNode*> values;
 ObJsonNodeType json_type()const override{return ObJsonNodeType::J_OBJECT;}
 int add(const char*k,ObJsonNode*v){values[k]=v;return 0;}
 ObJsonNode*get_value(const char*k){auto i=values.find(k);return i==values.end()?nullptr:i->second;}
 int seek(const struct ObJsonPath&,long,bool,bool,struct ObJsonSeekResult&);
};
struct ObJsonArray:ObIJsonBase {
 std::vector<ObJsonNode*> values;
 ObJsonNodeType json_type()const override{return ObJsonNodeType::J_ARRAY;}
 long element_count()const{return values.size();}
 ObJsonNode*get_value(long n){return values.at(n);}
 int append(ObJsonNode*v){values.push_back(v);return 0;}
};
struct ObJsonPath {std::string key;ObJsonPath(ObString s,ObIAllocator*):key(s.value.substr(2)){} int parse_path(){return 0;} long path_node_cnt(){return 1;}};
struct ObJsonSeekResult:std::vector<ObJsonNode*>{};
int ObJsonObject::seek(const ObJsonPath&p,long,bool,bool,ObJsonSeekResult&h){auto*n=get_value(p.key.c_str());if(n)h.push_back(n);return 0;}
int ob_write_string(ObIAllocator&,const ObString&in,ObString&out,bool){out=in;return 0;}
}
using namespace common;
struct ObAIFuncJsonUtils {
 static int get_json_object(ObIAllocator&,ObJsonObject*&o){o=new ObJsonObject;return 0;}
 static int get_json_string(ObIAllocator&,ObString&s,ObJsonString*&o){o=new ObJsonString(s);return 0;}
 static int get_json_boolean(ObIAllocator&,bool b,ObJsonBoolean*&o){o=new ObJsonBoolean(b);return 0;}
 static int transform_array_to_json_array(ObIAllocator&,ObArray<ObString>&v,ObJsonArray*&o){o=new ObJsonArray;for(auto&s:v)o->append(new ObJsonString(s));return 0;}
 static int compact_json_object(ObIAllocator&,ObJsonObject*from,ObJsonObject*to){if(from)for(auto&v:from->values)to->values[v.first]=v.second;return 0;}
};
struct ObAIFuncIComplete{virtual ~ObAIFuncIComplete()=default;};
struct ObAIFuncIEmbed{virtual ~ObAIFuncIEmbed()=default;};
struct ObOllamaUtils {
 static int get_header(ObIAllocator&,ObArray<ObString>&);
 struct ObOllamaComplete:ObAIFuncIComplete {
 static int get_header(ObIAllocator&,ObString&,ObArray<ObString>&);
 static int get_body(ObIAllocator&,ObString&,ObString&,ObString&,ObJsonObject*,ObJsonObject*&);
 static int set_config_json_format(ObIAllocator&,ObJsonObject*);
 static int parse_output(ObIAllocator&,ObJsonObject*,ObIJsonBase*&);
 };
 struct ObOllamaEmbed:ObAIFuncIEmbed {
 static int get_header(ObIAllocator&,ObString&,ObArray<ObString>&);
 static int get_body(ObIAllocator&,ObString&,ObArray<ObString>&,ObJsonObject*,ObJsonObject*&);
 static int parse_output(ObIAllocator&,ObJsonObject*,ObIJsonBase*&);
 };
};
struct ObOpenAIUtils{struct ObOpenAIComplete:ObAIFuncIComplete{};struct ObOpenAIEmbed:ObAIFuncIEmbed{};};
struct ObDashscopeUtils{struct ObDashscopeComplete:ObAIFuncIComplete{};struct ObDashscopeEmbed:ObAIFuncIEmbed{};};
struct ObAIFuncProviderUtils {
 static constexpr auto OPENAI="OPENAI",ALIYUN="ALIYUN-OPENAI",DEEPSEEK="DEEPSEEK",SILICONFLOW="SILICONFLOW",HUNYUAN="HUNYUAN-OPENAI",DASHSCOPE="ALIYUN-DASHSCOPE",OLLAMA="OLLAMA";
};
struct ObAIFuncUtils {
 static bool ob_provider_check(const ObString&p,const char*s){return p.case_compare(s)==0;}
 static int get_complete_provider(ObIAllocator&,const ObString&,ObAIFuncIComplete*&);
 static int get_embed_provider(ObIAllocator&,const ObString&,ObAIFuncIEmbed*&);
};
struct ObAiModelEndpointInfo {
 ObString name_="ep",scope_="ALL",ai_model_name_="m",url_="http://local",access_key_,provider_,parameters_,request_transform_fn_,response_transform_fn_;
 static const ObString DEFAULT_SCOPE;
 static bool is_valid_provider(const ObString&);
 int check_valid()const;
};
const ObString ObAiModelEndpointInfo::DEFAULT_SCOPE="ALL";
'''

CASES = r'''
int main(int argc,char**argv){
 assert(argc==2);std::string c=argv[1];ObIAllocator a;ObString model="custom-model",system="",content="Unicode: 中文";
 if(c=="metadata") {
  ObAiModelEndpointInfo ep;
  for(auto*p:{"ollama","Ollama","OLLAMA"}){ep.provider_=p;assert(ep.check_valid()==0);}
  ep.provider_="openai";assert(ep.check_valid()==OB_AI_FUNC_PARAM_EMPTY);
  ep.access_key_="key";assert(ep.check_valid()==0);
  ep.provider_="unknown";assert(ep.check_valid()==OB_AI_FUNC_PARAM_VALUE_INVALID);
 }else if(c=="dispatch") {
  for(auto*p:{"ollama","OlLaMa","OLLAMA"}){ObAIFuncIComplete*c=nullptr;ObAIFuncIEmbed*e=nullptr;
   assert(ObAIFuncUtils::get_complete_provider(a,p,c)==0);assert(dynamic_cast<ObOllamaUtils::ObOllamaComplete*>(c));delete c;
   assert(ObAIFuncUtils::get_embed_provider(a,p,e)==0);assert(dynamic_cast<ObOllamaUtils::ObOllamaEmbed*>(e));delete e;}
 }else if(c=="nonstream") {
  ObJsonObject*body=nullptr;ObJsonObject config;ObJsonBoolean yes(true);config.add("stream",&yes);
  assert(ObOllamaUtils::ObOllamaComplete::get_body(a,model,system,content,&config,body)==0);
  auto*s=body->get_value("stream");assert(s&&s->json_type()==ObJsonNodeType::J_BOOLEAN&&!static_cast<ObJsonBoolean*>(s)->value);
  assert(static_cast<ObJsonString*>(body->get_value("model"))->value=="custom-model");
  assert(static_cast<ObJsonString*>(body->get_value("prompt"))->value=="Unicode: 中文");
 }else if(c=="headers") {
  ObString empty;ObArray<ObString>h;assert(ObOllamaUtils::ObOllamaComplete::get_header(a,empty,h)==0);
  assert(h.size()==1&&h[0].value=="Content-Type: application/json");
 }else if(c=="completion_response") {
  ObIJsonBase*out=nullptr;ObJsonObject r;ObString empty="",text="valid";ObJsonString bad(empty),good(text);ObJsonArray wrong;
  r.add("response",&bad);assert(ObOllamaUtils::ObOllamaComplete::parse_output(a,&r,out)==OB_INVALID_DATA);
  r.add("response",&wrong);assert(ObOllamaUtils::ObOllamaComplete::parse_output(a,&r,out)==OB_INVALID_DATA);
  r.add("response",&good);assert(ObOllamaUtils::ObOllamaComplete::parse_output(a,&r,out)==0&&out==&good);
 }else if(c=="embedding_response") {
  ObIJsonBase*out=nullptr;ObJsonObject r;ObJsonArray vectors,vector;ObJsonString wrong(content);
  r.add("embeddings",&wrong);assert(ObOllamaUtils::ObOllamaEmbed::parse_output(a,&r,out)==OB_INVALID_DATA);
  r.add("embeddings",&vectors);vectors.append(&wrong);assert(ObOllamaUtils::ObOllamaEmbed::parse_output(a,&r,out)==OB_INVALID_DATA);
  vectors.values[0]=&vector;assert(ObOllamaUtils::ObOllamaEmbed::parse_output(a,&r,out)==0&&out==&vectors);
 }else if(c=="embedding_body") {
  ObArray<ObString> inputs;inputs.push_back("first");inputs.push_back("second");ObJsonObject*body=nullptr;
  assert(ObOllamaUtils::ObOllamaEmbed::get_body(a,model,inputs,nullptr,body)==0);
  auto*v=static_cast<ObJsonArray*>(body->get_value("input"));assert(v&&v->element_count()==2);
  assert(static_cast<ObJsonString*>(v->get_value(1))->value=="second");
 }
}
'''


class OllamaComponents(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        utils = (ROOT / "src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp").read_text()
        endpoint = (ROOT / "src/share/ai_service/ob_ai_service_struct.cpp").read_text()
        pieces = [utils[utils.index("int ObOllamaUtils::get_header"):utils.index("int ObDashscopeUtils::get_header")],
                  utils[utils.index("int ObAIFuncUtils::get_complete_provider"):utils.index("int ObAIFuncUtils::get_rerank_provider")],
                  endpoint[endpoint.index("const char *VALID_PROVIDERS"):endpoint.index("#define EXTRACT_JSON_ELEM_STR")],
                  endpoint[endpoint.index("int ObAiModelEndpointInfo::check_valid"):endpoint.index("int ObAiModelEndpointInfo::merge_delta_endpoint")]]
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        source = pathlib.Path(cls.directory.name) / "ollama.cpp"
        cls.executable = source.with_suffix("")
        source.write_text(SCAFFOLD + "\n".join(pieces) + CASES)
        subprocess.run(["g++", "-std=c++17", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                        str(source), "-o", str(cls.executable)], check=True)

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True)

    def test_metadata(self): self.run_case("metadata")
    def test_dispatch(self): self.run_case("dispatch")
    def test_nonstream(self): self.run_case("nonstream")
    def test_headers(self): self.run_case("headers")
    def test_completion_response(self): self.run_case("completion_response")
    def test_embedding_response(self): self.run_case("embedding_response")
    def test_embedding_body(self): self.run_case("embedding_body")
