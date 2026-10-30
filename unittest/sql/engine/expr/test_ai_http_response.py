"""Production HTTP result handling with curl/parser/allocator boundary doubles."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]

SCAFFOLD = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT -1
#define OB_INIT_FAIL -2
#define OB_TIMEOUT -3
#define OB_CURL_ERROR -4
#define OB_ERR_INVALID_JSON_TEXT -5
#define OB_INVALID_DATA -6
#define OB_ISNULL(x) ((x)==nullptr)
#define OB_SUCC(x) ((x)==0)
#define OB_FAIL(x) ((ret=(x))!=0)
#define LOG_WARN(...)
#define FORWARD_USER_ERROR(...)
struct ObIAllocator{};
struct ObString{std::string value;ObString()=default;ObString(const char*s):value(s){}const char*ptr()const{return value.c_str();}void reset(){value.clear();}};
struct ObStringBuffer{explicit ObStringBuffer(ObIAllocator*){}ObString string()const{return "response";}void reset(){}};
struct ObJsonBuffer:ObStringBuffer{using ObStringBuffer::ObStringBuffer;};
enum class ObJsonNodeType{J_OBJECT,J_ARRAY,J_STRING,J_NULL};
struct ObIJsonBase{virtual ~ObIJsonBase()=default;virtual ObJsonNodeType json_type()const=0;virtual int print(ObJsonBuffer&,int){return 0;}};
struct ObJsonObject:ObIJsonBase{ObJsonNodeType json_type()const override{return ObJsonNodeType::J_OBJECT;}};
struct ObJsonArray:ObIJsonBase{ObJsonNodeType json_type()const override{return ObJsonNodeType::J_ARRAY;}};
struct ObJsonString:ObIJsonBase{ObJsonNodeType json_type()const override{return ObJsonNodeType::J_STRING;}};
struct ObJsonNull:ObIJsonBase{ObJsonNodeType json_type()const override{return ObJsonNodeType::J_NULL;}};
enum class ObJsonInType{JSON_TREE};
static ObIJsonBase*parsed=nullptr;
struct ObJsonBaseFactory{static int get_json_base(ObIAllocator*,ObString,ObJsonInType,ObJsonInType,ObIJsonBase*&out){out=parsed;return 0;}};
struct ObAIFuncJsonUtils{static int print_json_to_str(ObIAllocator&,ObIJsonBase*,ObString&);};
template<class T>struct ObArray:std::vector<T>{long count()const{return this->size();}int push_back(T v){std::vector<T>::push_back(v);return 0;}};
int ob_write_string(ObIAllocator&,ObString in,ObString&out){out=in;return 0;}
using CURLcode=int;struct CURL{};struct CURLM{};
static constexpr int CURLE_OK=0,CURLINFO_RESPONSE_CODE=1,CURLMSG_DONE=2;
static int transport_code=0,info_code=0;static long status_code=200;static bool message_pending=false;
struct CURLMsg{int msg=CURLMSG_DONE;CURL*easy_handle=nullptr;struct{CURLcode result=0;}data;};
CURL*curl_easy_init(){static CURL handle;return &handle;}
CURLcode curl_easy_perform(CURL*){return transport_code;}
int curl_easy_getinfo(CURL*,int,long*out){*out=status_code;return info_code;}
CURLMsg*curl_multi_info_read(CURLM*,int*){static CURLMsg message;if(!message_pending)return nullptr;message_pending=false;message.data.result=transport_code;return &message;}
void ob_usleep(long){}
struct ObAIFuncClient {
 ObIAllocator alloc;ObIAllocator*allocator_=&alloc;CURL*curl_=nullptr;CURLM*curlm_=nullptr;
 long max_retry_times_=0;std::atomic<bool>is_finished_{true};ObArray<ObStringBuffer*>response_buffers_;
 int init_easy_handle(CURL*,ObJsonObject*,ObStringBuffer&){return 0;}
 bool is_timeout(){return false;}bool is_retryable_status_code(long){return false;}
 int error_handle(int){return OB_CURL_ERROR;}
 int send_post(ObJsonObject*,ObJsonObject*&);
 int get_batch_result(ObArray<ObJsonObject*>&);
};
'''

CASES = r'''
int main(int argc,char**argv){
 assert(argc==2);std::string name=argv[1];ObJsonObject object,request;ObJsonArray array;ObJsonString string;ObJsonNull null;
 parsed=&object;bool success=true;
 if(name.find("array")!=std::string::npos){parsed=&array;success=false;}
 if(name.find("string")!=std::string::npos){parsed=&string;success=false;}
 if(name.find("null")!=std::string::npos){parsed=&null;success=false;}
 if(name.find("null_ptr")!=std::string::npos)parsed=nullptr;
 if(name.find("transport")!=std::string::npos){transport_code=18;success=false;}
 if(name.find("info")!=std::string::npos){info_code=1;success=false;}
 if(name.find("created")!=std::string::npos)status_code=201;
 if(name.find("http")!=std::string::npos){status_code=403;success=false;}
 ObAIFuncClient client;
 if(name.find("scalar_")==0){
  ObJsonObject*out=&object;int ret=client.send_post(&request,out);
  if(success)assert(ret==0&&out==&object);else assert(ret!=0&&out==nullptr);
 }else{
  ObStringBuffer buffer(&client.alloc);client.response_buffers_.push_back(&buffer);message_pending=true;
  ObArray<ObJsonObject*>out;int ret=client.get_batch_result(out);
  if(success)assert(ret==0&&out.size()==1&&out[0]==&object);else assert(ret!=0&&out.empty());
 }
}
'''


class HTTPResponse(unittest.TestCase):
    def test_null_response_error_reporting(self):
        source = (ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp').read_text()
        actual = source[source.index('int ObAIFuncJsonUtils::print_json_to_str('):
                        source.index('int ObAIFuncJsonUtils::get_json_object_form_str(')]
        main = 'int main(){ObIAllocator allocator;ObString out;assert(ObAIFuncJsonUtils::print_json_to_str(allocator,nullptr,out)==OB_INVALID_ARGUMENT);}'
        with tempfile.TemporaryDirectory() as directory:
            cpp = pathlib.Path(directory) / 'printer.cpp'
            executable = cpp.with_suffix('')
            cpp.write_text(SCAFFOLD + actual + main)
            subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            str(cpp), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_root_types_and_transport_status(self):
        source = (ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_client.cpp').read_text()
        first = source[source.index('int ObAIFuncClient::send_post(ObJsonObject *data,'):
                       source.index('int ObAIFuncClient::send_post(common::ObIAllocator')]
        batch = source[source.index('int ObAIFuncClient::get_batch_result('):
                       source.index('int ObAIFuncClient::init_easy_handle(')]
        with tempfile.TemporaryDirectory() as directory:
            cpp = pathlib.Path(directory) / 'client.cpp'
            executable = cpp.with_suffix('')
            cpp.write_text(SCAFFOLD + first + batch + CASES)
            subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            str(cpp), '-o', str(executable)], check=True)
            for case in ('scalar_object', 'scalar_array', 'scalar_string', 'scalar_null', 'scalar_null_ptr',
                         'scalar_transport', 'scalar_created', 'scalar_http', 'batch_object', 'batch_array',
                         'batch_string', 'batch_null', 'batch_null_ptr', 'batch_transport', 'batch_info',
                         'batch_created', 'batch_http'):
                with self.subTest(case=case):
                    subprocess.run([str(executable), case], check=True)
