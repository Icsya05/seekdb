"""Execute the actual provider parser with JSON/allocation boundary doubles."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]


class EmbeddingResponseTypes(unittest.TestCase):
    def test_provider_checks_array_types(self):
        text = (ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp').read_text()
        start = text.index('int ObOpenAIUtils::ObOpenAIEmbed::parse_output(')
        end = text.index('int ObOllamaUtils::get_header', start)
        scaffold = r'''
#include <map>
#include <string>
#include <vector>
#include <cassert>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT -1
#define OB_INVALID_DATA -2
#define OB_ISNULL(x) ((x)==nullptr)
#define OB_SUCC(x) ((x)==0)
#define OB_FAIL(x) ((ret=(x))!=0)
namespace common {
enum class ObJsonNodeType {J_OBJECT,J_ARRAY,J_STRING};
struct ObIAllocator {};
struct ObIJsonBase {virtual ~ObIJsonBase()=default; virtual ObJsonNodeType json_type()=0;};
using ObJsonNode=ObIJsonBase;
struct ObJsonObject:ObIJsonBase {
 std::map<std::string,ObJsonNode*> values;
 ObJsonNodeType json_type() override{return ObJsonNodeType::J_OBJECT;}
 ObJsonNode*get_value(const char*k){auto i=values.find(k);return i==values.end()?nullptr:i->second;}
};
struct ObJsonArray:ObIJsonBase {
 std::vector<ObJsonNode*> values;
 ObJsonNodeType json_type() override{return ObJsonNodeType::J_ARRAY;}
 long element_count(){return values.size();}
 ObJsonNode*get_value(long i){return values.at(i);}
 int append(ObJsonNode*n){values.push_back(n);return 0;}
};
struct String:ObIJsonBase {ObJsonNodeType json_type()override{return ObJsonNodeType::J_STRING;}};
}
using namespace common;
struct ObAIFuncJsonUtils {
 static int get_json_array(ObIAllocator&,ObJsonArray*&out){out=new ObJsonArray;return 0;}
};
struct ObOpenAIUtils {struct ObOpenAIEmbed {
 static int parse_output(ObIAllocator&,ObJsonObject*,ObIJsonBase*&);
};};
'''
        cases = r'''
int main(){
 ObIAllocator a; ObJsonObject response,item; ObJsonArray data,vector;
 response.values["data"]=&data;data.values.push_back(&item);item.values["embedding"]=&vector;
 ObIJsonBase*out=nullptr;
 assert(ObOpenAIUtils::ObOpenAIEmbed::parse_output(a,&response,out)==0);delete out;
 response.values["data"]=&item;out=nullptr;
 assert(ObOpenAIUtils::ObOpenAIEmbed::parse_output(a,&response,out)==OB_INVALID_DATA);
 response.values["data"]=&data;String scalar;item.values["embedding"]=&scalar;out=nullptr;
 assert(ObOpenAIUtils::ObOpenAIEmbed::parse_output(a,&response,out)==OB_INVALID_DATA);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = pathlib.Path(directory) / 'parser.cpp'
            exe = pathlib.Path(directory) / 'parser'
            cpp.write_text(scaffold + text[start:end] + cases)
            subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined',
                            '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
