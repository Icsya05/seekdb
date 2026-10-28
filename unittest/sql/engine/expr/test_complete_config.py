"""Execute the production completion evaluator with SQL/JSON/transport boundary doubles.

This checks evaluator control flow, not real SQL datum or LOB representation.
Opt-in server tests separately exercise those integration boundaries.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]

SCAFFOLD = r'''
#include <cassert>
#include <string>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT -1
#define OB_ERR_UNEXPECTED -2
#define INIT_SUCC(x) int x=0
#define OB_FAIL(x) ((ret=(x))!=0)
#define OB_SUCC(x) ((x)==0)
#define OB_ISNULL(x) ((x)==nullptr)
#define OB_NOT_NULL(x) ((x)!=nullptr)
#define OB_FALSE_IT(x) ((void)(x),false)
#define LOG_USER_ERROR(...)
#define N_AI_COMPLETE 0
enum ObObjType {ObVarcharType,ObJsonType};
enum class ObJsonNodeType {J_OBJECT,J_ARRAY};
struct ObString {
 std::string value;
 ObString()=default; ObString(const char*s):value(s){}
 bool empty()const{return value.empty();}
};
struct ObIAllocator {};
struct ObIJsonBase {virtual ~ObIJsonBase()=default; virtual ObJsonNodeType json_type()const=0;};
struct ObJsonObject:ObIJsonBase {ObJsonNodeType json_type()const override{return ObJsonNodeType::J_OBJECT;}};
struct ObJsonArray:ObIJsonBase {ObJsonNodeType json_type()const override{return ObJsonNodeType::J_ARRAY;}};
struct ObDatum {
 bool null=false; ObString text; ObIJsonBase*json=nullptr;
 bool is_null()const{return null;} void set_null(){null=true;}
 ObString get_string()const{assert(!null);return text;}
};
struct ObEvalCtx {
 void*exec_ctx_=nullptr;
 struct TempAllocGuard {ObIAllocator alloc;TempAllocGuard(ObEvalCtx&){} ObIAllocator&get_allocator(){return alloc;}};
};
struct MultimodeAlloctor:ObIAllocator {explicit MultimodeAlloctor(ObIAllocator&) {}};
namespace lib {struct ObMemAttr {explicit ObMemAttr(int){}};struct ObMallocHookAttrGuard{explicit ObMallocHookAttrGuard(ObMemAttr){}};}
struct ObExpr {
 struct Meta{ObObjType type_=ObVarcharType;} datum_meta_;
 struct ObjMeta{bool has_lob_header()const{return false;}} obj_meta_;
 ObExpr*args_[3]{};ObDatum*values[3]{};
 int eval_param_value(ObEvalCtx&,ObDatum*&m,ObDatum*&p,ObDatum*&c)const{m=values[0];p=values[1];c=values[2];return 0;}
};
struct ObAIFuncExprInfo {};
namespace oceanbase {
namespace share {struct ObAiModelEndpointInfo{};template<class T>T*server_service(){static T service;return &service;}}
namespace query {struct ObIAiEndpointResolver{int resolve_by_model_name(ObString&,ObIAllocator&,share::ObAiModelEndpointInfo&){return 0;}};}
}
namespace share=oceanbase::share;
namespace query=oceanbase::query;
static int calls=0;
static ObJsonObject*observed_config=nullptr;
struct ObAIFuncUtils {
 static int get_ai_func_info(ObIAllocator&,ObString&,ObAIFuncExprInfo*&out){static ObAIFuncExprInfo info;out=&info;return 0;}
 static int set_string_result(const ObExpr&,ObEvalCtx&,ObDatum&out,ObString&value){out.text=value;out.null=false;return 0;}
};
struct ObAIFuncModel {
 ObAIFuncModel(ObIAllocator&,ObAIFuncExprInfo&,const share::ObAiModelEndpointInfo&){}
 int call_completion(ObString&,ObJsonObject*config,ObString&out){++calls;observed_config=config;out="valid completion";return 0;}
};
struct ObTextStringHelper {
 static int read_real_string_data(void*,ObIAllocator&,ObDatum&datum,ObExpr::Meta,bool,ObString&out){
  if(datum.is_null())return OB_INVALID_ARGUMENT;
  out=datum.json?ObString("binary-json-not-text"):datum.text;return 0;
 }
};
struct ObJsonExprHelper {
 static int get_json_doc(const ObExpr&expr,ObEvalCtx&,ObIAllocator&,int index,ObIJsonBase*&out,bool&is_null){
  is_null=expr.values[index]->is_null();out=expr.values[index]->json;
  if(!out&&expr.values[index]->text.value=="{}"){static ObJsonObject object;out=&object;}
  return out?0:OB_INVALID_ARGUMENT;
 }
};
struct ObAIFuncJsonUtils {
 static int get_json_object_form_str(ObIAllocator&,ObString&text,ObJsonObject*&out){
  if(text.value!="{}")return OB_INVALID_ARGUMENT;
  static ObJsonObject config;out=&config;return 0;
 }
};
struct ObAIFuncPromptObjectUtils {
 static bool is_valid_prompt_object(ObJsonObject*){return true;}
 static int replace_all_str_args_in_template(ObIAllocator&,ObJsonObject*,ObString&out){out="rendered prompt";return 0;}
};
struct ObExprAIComplete {
 static constexpr int PROMPT_IDX=1,CONFIG_IDX=2;
 static int eval_ai_complete(const ObExpr&,ObEvalCtx&,ObDatum&);
};
'''

CASES = r'''
int main(int argc,char**argv){
 assert(argc==2);std::string test=argv[1];
 ObExpr model_expr,prompt_expr,config_expr,expr;ObDatum model,prompt,config,result;ObEvalCtx ctx;
 model.text="arbitrary model";prompt.text="Unicode 中文 prompt";config.text="{}";
 expr.args_[0]=&model_expr;expr.args_[1]=&prompt_expr;expr.args_[2]=&config_expr;
 expr.values[0]=&model;expr.values[1]=&prompt;expr.values[2]=&config;
 ObJsonObject object;ObJsonArray array;
 bool expected_success=true;
 if(test=="omitted")expr.values[2]=nullptr;
 else if(test=="null")config.null=true;
 else if(test=="native_object"){config.json=&object;config_expr.datum_meta_.type_=ObJsonType;}
 else if(test=="native_array"){config.json=&array;config_expr.datum_meta_.type_=ObJsonType;expected_success=false;}
 else if(test=="invalid_string"){config.text="not json";expected_success=false;}
 else if(test=="prompt_object"){prompt.json=&object;prompt_expr.datum_meta_.type_=ObJsonType;}
 else assert(test=="text_object");
 int ret=ObExprAIComplete::eval_ai_complete(expr,ctx,result);
 if(expected_success){
  assert(ret==OB_SUCCESS&&calls==1&&result.text.value=="valid completion");
  if(test=="null"||test=="omitted")assert(observed_config==nullptr);
  if(test=="native_object")assert(observed_config==&object);
 }else assert(ret!=OB_SUCCESS&&calls==0);
}
'''


class CompletionConfig(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp').read_text()
        actual = source[source.index('int ObExprAIComplete::eval_ai_complete('):
                        source.index('int ObExprAIComplete::cg_expr(')]
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = pathlib.Path(cls.temp.name) / 'completion.cpp'
        cls.executable = cpp.with_suffix('')
        cpp.write_text(SCAFFOLD + actual + CASES)
        subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(cpp), '-o', str(cls.executable)], check=True)

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True)

    def test_omitted(self): self.run_case('omitted')
    def test_null(self): self.run_case('null')
    def test_native_object(self): self.run_case('native_object')
    def test_native_array(self): self.run_case('native_array')
    def test_text_object(self): self.run_case('text_object')
    def test_invalid_string(self): self.run_case('invalid_string')
    def test_prompt_object(self): self.run_case('prompt_object')
