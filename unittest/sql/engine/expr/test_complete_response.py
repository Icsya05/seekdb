"""Production OpenAI completion parser with JSON boundary doubles."""
import pathlib
import subprocess
import tempfile
import unittest

from test_ollama_components import SCAFFOLD

ROOT = pathlib.Path(__file__).resolve().parents[4]

CASES = r'''
int main(int argc,char**argv){
 assert(argc==2);std::string test=argv[1];ObIAllocator allocator;ObJsonObject response;
 ObString empty="",text="Unicode 中文 completion";ObJsonString bad(empty),good(text);ObJsonArray nonstring;
 ObIJsonBase*out=&good;
 if(test=="empty")response.add("choices[0].message.content",&bad);
 else if(test=="nonstring")response.add("choices[0].message.content",&nonstring);
 else if(test=="valid")response.add("choices[0].message.content",&good);
 else assert(test=="missing"||test=="null_response");
 int ret=ObOpenAIUtils::ObOpenAIComplete::parse_output(allocator,test=="null_response"?nullptr:&response,out);
 if(test=="valid")assert(ret==OB_SUCCESS&&out==&good);
 else {assert(ret==(test=="null_response"?OB_INVALID_ARGUMENT:OB_INVALID_DATA));assert(out==nullptr);}
}
'''


class CompletionResponse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp').read_text()
        actual = source[source.index('int ObOpenAIUtils::ObOpenAIComplete::parse_output('):
                        source.index('int ObOpenAIUtils::ObOpenAIEmbed::get_header(')]
        scaffold = SCAFFOLD.replace('struct ObOpenAIComplete:ObAIFuncIComplete{};',
                                   'struct ObOpenAIComplete:ObAIFuncIComplete{'
                                   'static int parse_output(ObIAllocator&,ObJsonObject*,ObIJsonBase*&);};')
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = pathlib.Path(cls.temp.name) / 'response.cpp'
        cls.executable = cpp.with_suffix('')
        cpp.write_text(scaffold + actual + CASES)
        subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(cpp), '-o', str(cls.executable)], check=True)

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True)

    def test_empty(self): self.run_case('empty')
    def test_nonstring(self): self.run_case('nonstring')
    def test_missing(self): self.run_case('missing')
    def test_null_response(self): self.run_case('null_response')
    def test_valid(self): self.run_case('valid')
