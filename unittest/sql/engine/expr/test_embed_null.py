"""Run the production scalar evaluator with SQL/transport boundary doubles.

The separate opt-in SQL suite covers actual nullable SQL datums and row sets.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]

SCAFFOLD = r'''
#include <cassert>
#include <cstdint>
#include <string>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT -1
#define OB_ERR_UNEXPECTED -2
#define INIT_SUCC(x) int x=0
#define OB_FAIL(x) ((ret=(x))!=0)
#define OB_NOT_NULL(x) ((x)!=nullptr)
#define OB_ISNULL(x) ((x)==nullptr)
#define LOG_USER_ERROR(...)
#define N_AI_EMBED 0
struct ObString {
 std::string value;
 ObString()=default; ObString(const char*s):value(s){}
 bool empty()const{return value.empty();}
};
struct ObIAllocator {};
struct ObDatum {
 bool null=false; ObString text; int64_t integer=0;
 bool is_null()const{return null;} void set_null(){null=true;}
 ObString get_string()const{assert(!null);return text;}
 int64_t get_int()const{assert(!null);return integer;}
};
struct ObEvalCtx {
 struct TempAllocGuard {ObIAllocator alloc;TempAllocGuard(ObEvalCtx&){} ObIAllocator&get_allocator(){return alloc;}};
};
struct MultimodeAlloctor:ObIAllocator {explicit MultimodeAlloctor(ObIAllocator&) {}};
namespace lib {struct ObMemAttr {explicit ObMemAttr(int){}};struct ObMallocHookAttrGuard{explicit ObMallocHookAttrGuard(ObMemAttr){}};}
struct ObExpr {
 int arg_cnt_=2; ObDatum*values[3]{};
 int eval_param_value(ObEvalCtx&,ObDatum*&m,ObDatum*&c)const{m=values[0];c=values[1];return 0;}
 int eval_param_value(ObEvalCtx&ctx,ObDatum*&m,ObDatum*&c,ObDatum*&d)const{d=values[2];return eval_param_value(ctx,m,c);}
};
struct ObJsonInt {int64_t value=0;};
struct ObJsonObject {int64_t dimension=0;int add(const char*,ObJsonInt*d){dimension=d->value;return 0;}};
struct ObAIFuncExprInfo {};
namespace oceanbase {
namespace share {struct ObAiModelEndpointInfo{};template<class T>T*server_service(){static T service;return &service;}}
namespace query {struct ObIAiEndpointResolver{int resolve_by_model_name(ObString&,ObIAllocator&,share::ObAiModelEndpointInfo&){return 0;}};}
}
namespace share=oceanbase::share;
namespace query=oceanbase::query;
static int calls=0,lookups=0;
static int64_t observed_dimension=0;
struct ObAIFuncUtils {
 static int get_ai_func_info(ObIAllocator&,ObString&,ObAIFuncExprInfo*&out){++lookups;static ObAIFuncExprInfo info;out=&info;return 0;}
 static int set_string_result(const ObExpr&,ObEvalCtx&,ObDatum&out,ObString&value){out.text=value;out.null=false;return 0;}
};
struct ObAIFuncJsonUtils {
 static int get_json_object(ObIAllocator&,ObJsonObject*&out){static ObJsonObject object;out=&object;return 0;}
 static int get_json_int(ObIAllocator&,int64_t value,ObJsonInt*&out){static ObJsonInt number;number.value=value;out=&number;return 0;}
};
struct ObAIFuncModel {
 ObAIFuncModel(ObIAllocator&,ObAIFuncExprInfo&,const share::ObAiModelEndpointInfo&){}
 int call_dense_embedding(ObString&content,ObJsonObject*config,ObString&out){
  assert(content.value=="Unicode 中文 content");++calls;observed_dimension=config?config->dimension:0;out="[0.25,0.5]";return 0;
 }
};
struct ObExprAIEmbed {static int eval_ai_embed(const ObExpr&,ObEvalCtx&,ObDatum&);};
'''

CASES = r'''
int main(int argc,char**argv){
 assert(argc==2);std::string test=argv[1];
 ObExpr expr;ObDatum model,content,dim,result;ObEvalCtx ctx;
 model.text="model";content.text="Unicode 中文 content";dim.integer=2;
 expr.values[0]=&model;expr.values[1]=&content;expr.values[2]=&dim;
 bool expect_null=false,expect_error=false;
 if(test=="null_content"){content.null=true;expect_null=true;}
 else if(test=="null_content_with_dimension"){content.null=true;expr.arg_cnt_=3;expect_null=true;}
 else if(test=="null_model"){model.null=true;expect_error=true;}
 else if(test=="empty_content"){content.text="";expect_error=true;}
 else if(test=="empty_model"){model.text="";expect_error=true;}
 else if(test=="explicit_dimension")expr.arg_cnt_=3;
 else if(test=="null_dimension"){expr.arg_cnt_=3;dim.null=true;expect_error=true;}
 else if(test=="zero_dimension"){expr.arg_cnt_=3;dim.integer=0;expect_error=true;}
 else if(test=="negative_dimension"){expr.arg_cnt_=3;dim.integer=-1;expect_error=true;}
 else assert(test=="default_dimension");
 int ret=ObExprAIEmbed::eval_ai_embed(expr,ctx,result);
 if(expect_error)assert(ret==OB_INVALID_ARGUMENT&&calls==0&&lookups==0);
 else if(expect_null)assert(ret==OB_SUCCESS&&result.is_null()&&calls==0&&lookups==0);
 else assert(ret==OB_SUCCESS&&!result.is_null()&&result.text.value=="[0.25,0.5]"&&calls==1&&
             observed_dimension==(test=="explicit_dimension"?2:0));
}
'''


class EmbedNull(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.cpp').read_text()
        actual = source[source.index('int ObExprAIEmbed::eval_ai_embed('):
                        source.index('int ObExprAIEmbed::cg_expr(')]
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = pathlib.Path(cls.temp.name) / 'embed.cpp'
        cls.executable = cpp.with_suffix('')
        cpp.write_text(SCAFFOLD + actual + CASES)
        subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(cpp), '-o', str(cls.executable)], check=True)

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True)

    def test_null_content(self): self.run_case('null_content')
    def test_null_content_with_dimension(self): self.run_case('null_content_with_dimension')
    def test_null_model(self): self.run_case('null_model')
    def test_empty_content(self): self.run_case('empty_content')
    def test_empty_model(self): self.run_case('empty_model')
    def test_default_dimension(self): self.run_case('default_dimension')
    def test_explicit_dimension(self): self.run_case('explicit_dimension')
    def test_null_dimension(self): self.run_case('null_dimension')
    def test_zero_dimension(self): self.run_case('zero_dimension')
    def test_negative_dimension(self): self.run_case('negative_dimension')
