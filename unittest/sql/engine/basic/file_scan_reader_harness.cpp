#include "sql/engine/basic/ob_file_scan_utils.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
using namespace oceanbase::sql::file_sql;

static void write_file(const std::string &p, const std::string &s) { std::ofstream f(p, std::ios::binary); f << s; }
static void must(int actual, int expected, const std::string &error) {
  if (actual != expected) { std::cerr << "status " << actual << " expected " << expected << ": " << error << '\n'; std::abort(); }
}
int main(int argc, char **argv) {
  assert(argc == 2); const std::string mode = argv[1];
  char temporary[] = "/tmp/seekdb-reader-XXXXXX"; char *created = mkdtemp(temporary); assert(created);
  const std::string dir(created), path = dir + "/data.csv";
  struct Cleanup { std::string dir; ~Cleanup() { const std::string c = "rm -rf -- '" + dir + "'"; assert(system(c.c_str()) == 0); } } cleanup{dir};
  Options options; Binding binding; Scanner scanner; std::string error; std::vector<Cell> row;
  auto infer = [&](Format format = Format::CSV) { return infer_file(path, dir, format, options, binding, error); };
  auto open = [&]() { must(scanner.open(binding, dir, error), OK, error); };
  auto next = [&]() { must(scanner.next(row, error), OK, error); };
  if (mode == "csv") {
    write_file(path, "\xef\xbb\xbfid,name,note,empty\r\n1,中文,\"line1\r\nline2\",\r\n2,\"a,b\",\"say \"\"hi\"\"\",\"\"\r\n");
    must(infer(), OK, error); assert(binding.columns.size() == 4 && binding.columns[0].type == Type::BIGINT);
    assert(binding.columns[3].type == Type::VARCHAR && binding.columns[3].nullable);
    open(); next(); assert(row[1].text == "中文" && row[2].text == "line1\r\nline2" && row[3].is_null);
    next(); assert(row[1].text == "a,b" && row[2].text == "say \"hi\"" && !row[3].is_null && row[3].text.empty());
    must(scanner.next(row,error), END,error);
  } else if (mode == "options") {
    options.delimiter = ';'; options.quote = '\''; options.escape = '\\'; options.nullstr = "NULL";
    write_file(path, "a;b\n'it\\'s';NULL\n'NULL';'slash\\\\end'\n"); must(infer(),OK,error); open();
    next(); assert(row[0].text == "it's" && row[1].is_null); next(); assert(!row[0].is_null && row[1].text == "slash\\end");
    options.sample_size=1; must(infer(),INVALID,error);
    options.sample_size=-1; options.delimiter=options.quote; must(infer(),INVALID,error);
  } else if (mode == "headers") {
    write_file(path,"id,id,ID,,id__2\n1,2,3,4,5\n"); must(infer(),OK,error);
    assert(binding.columns[0].name=="id" && binding.columns[1].name=="id__2" && binding.columns[2].name=="ID__3");
    assert(binding.columns[3].name=="column4" && binding.columns[4].name=="id__2__2");
  } else if (mode == "types") {
    options.header=false; write_file(path,"1,1,true,2024-02-29,2024-02-29 12:34:56,\n2,2.5,FALSE,2024-03-01,2024-03-01 01:02:03,\n");
    must(infer(),OK,error); assert(binding.columns.size()==6);
    const Type expected[]={Type::BIGINT,Type::DOUBLE,Type::BOOLEAN,Type::DATE,Type::DATETIME,Type::NULL_VALUE};
    for(size_t i=0;i<6;++i) assert(binding.columns[i].type==expected[i]);
    assert(binding.columns[0].name=="column1" && binding.columns[5].nullable); open(); next(); assert(row[0].text=="1");
  } else if (mode == "calendar") {
    write_file(path,"leap,bad,wide,huge,nonfinite,mix,timestamp\n2000-02-29,1900-02-29,9223372036854775808,1e999,NaN,2024-01-01,2024-01-01T23:59:59.123456\n2024-02-29,2024-04-31,1,2,inf,2024-01-01 01:02:03,2024-01-01 00:00:00\n");
    must(infer(),OK,error); const Type expected[]={Type::DATE,Type::VARCHAR,Type::DOUBLE,Type::VARCHAR,Type::VARCHAR,Type::DATETIME,Type::DATETIME};
    for(size_t i=0;i<7;++i) assert(binding.columns[i].type==expected[i]);
  } else if (mode == "full_schema") {
    {std::ofstream f(path); f<<"value\n";for(int i=0;i<10000;++i)f<<i<<'\n';f<<"last-is-text\n";}
    must(infer(),OK,error); assert(binding.columns[0].type==Type::VARCHAR && binding.columns[0].max_length==12);
  } else if (mode == "empty") {
    write_file(path,""); must(infer(),INVALID,error); write_file(path,"a,b\n");must(infer(),OK,error);
    assert(binding.columns[0].type==Type::NULL_VALUE && binding.columns[0].nullable);open();must(scanner.next(row,error),END,error);
  } else if (mode == "invalid") {
    for(const auto &s:std::vector<std::string>{"a,b\n1\n","a\n\"unterminated","a\n\"ok\"junk\n","a\nx\"y\n",std::string("a\n")+char(0xc0)+char(0x80)+"\n"}) {
      write_file(path,s);must(infer(),INVALID,error);assert(!error.empty());
    }
  } else if (mode == "limits") {
    write_file(path,"a\n"+std::string(8*1024*1024+1,'x'));must(infer(),LIMIT_EXCEEDED,error);
    std::string many;for(int i=0;i<1025;++i){if(i)many+=',';many+="a";}write_file(path,many+'\n');must(infer(),LIMIT_EXCEEDED,error);
  } else if (mode == "paths") {
    write_file(path,"a\n1\n");must(infer_file(path,"",Format::CSV,options,binding,error),DENIED,error);
    must(infer_file(path,std::string("/tmp\0junk",9),Format::CSV,options,binding,error),DENIED,error);
    must(infer_file(path+std::string("\0junk",5),dir,Format::CSV,options,binding,error),DENIED,error);
    must(infer_file(dir,dir,Format::CSV,options,binding,error),DENIED,error);
    assert(mkfifo((dir+"/fifo").c_str(),0600)==0);must(infer_file(dir+"/fifo",dir,Format::CSV,options,binding,error),DENIED,error);
    must(infer_file("../data.csv",dir,Format::CSV,options,binding,error),DENIED,error);
    must(infer_file("data.csv",dir,Format::CSV,options,binding,error),OK,error);assert(binding.path==path);
  } else if (mode == "symlinks") {
    write_file(path,"a\n1\n");assert(symlink("data.csv",(dir+"/link").c_str())==0);
    must(infer_file(dir+"/link",dir,Format::CSV,options,binding,error),DENIED,error);
    assert(symlink("/tmp",(dir+"/out").c_str())==0);must(infer_file(dir+"/out/other",dir,Format::CSV,options,binding,error),DENIED,error);
    must(infer_file(dir+"/../other",dir,Format::CSV,options,binding,error),DENIED,error);
    must(infer_file("/etc/passwd",dir,Format::CSV,options,binding,error),DENIED,error);
  } else if (mode == "directory_swap") {
    assert(mkdir((dir+"/inside").c_str(),0700)==0);write_file(dir+"/inside/a","v\n1\n");
    must(infer_file(dir+"/inside/a",dir,Format::CSV,options,binding,error),OK,error);
    assert(rename((dir+"/inside").c_str(),(dir+"/moved").c_str())==0);assert(symlink("/etc",(dir+"/inside").c_str())==0);
    must(scanner.open(binding,dir,error),DENIED,error);
  } else if (mode == "changed") {
    write_file(path,"a\n1\n2\n");must(infer(),OK,error);{std::ofstream f(path,std::ios::app);f<<"3\n";}
    must(scanner.open(binding,dir,error),CHANGED,error);must(infer(),OK,error);open();next();
    write_file(path,"a\n9\n2\n3\n");must(scanner.next(row,error),CHANGED,error);
  } else if (mode == "replaced") {
    write_file(path,"a\n1\n2\n");must(infer(),OK,error);open();next();assert(rename(path.c_str(),(path+".old").c_str())==0);
    write_file(path,"a\n1\n2\n");must(scanner.next(row,error),CHANGED,error);must(scanner.rescan(error),CHANGED,error);
  } else if (mode == "rescan") {
    write_file(path,"a\n1\n2\n");must(infer(),OK,error);open();next();assert(row[0].text=="1");
    must(scanner.rescan(error),OK,error);next();assert(row[0].text=="1");next();assert(row[0].text=="2");must(scanner.next(row,error),END,error);
    scanner.close();open();next();assert(row[0].text=="1");
  } else if (mode == "binding") {
    write_file(path,"a,b\n1,中文\n");must(infer(),OK,error);std::string json;must(serialize_binding(binding,json,error),OK,error);Binding copy;
    must(deserialize_binding(json,copy,error),OK,error);assert(copy.path==path&&copy.columns[1].name=="b"&&copy.columns[1].max_length==6);
    must(scanner.open(copy,dir,error),OK,error);next();assert(row[1].text=="中文");
    for(const std::string bad:{"{}","[]","null","{\"version\":9}","{\"path\":1}"})must(deserialize_binding(bad,copy,error),INVALID,error);
  } else if (mode == "jsonl") {
    write_file(path,"{\"id\":1,\"name\":\"中文\",\"active\":true,\"n\":null}\r\n{\"id\":2,\"extra\":2.5,\"name\":\"x\\ny\"}\n");
    must(infer(Format::JSONL),OK,error);assert(binding.columns.size()==5&&binding.columns[4].name=="extra");
    assert(binding.columns[0].type==Type::BIGINT&&binding.columns[2].type==Type::BOOLEAN&&binding.columns[3].type==Type::NULL_VALUE&&binding.columns[4].type==Type::DOUBLE);
    open();next();assert(row[1].text=="中文"&&row[2].text=="true"&&row[3].is_null&&row[4].is_null);
    next();assert(row[1].text=="x\ny"&&row[2].is_null&&row[4].text=="2.5");
  } else if (mode == "json_invalid") {
    for(const auto &s:std::vector<std::string>{"[]\n","null\n","{\"a\":{}}\n","{\"a\":[]}\n","{\"a\":1,\"a\":2}\n","{\"a\":NaN}\n","{\"a\":1} trailing\n","{\"a\":1}\n\n","{\"id\":1}\n{\"ID\":2}\n"}) {
      write_file(path,s);must(infer(Format::JSONL),INVALID,error);
    }
  } else if (mode == "json_union") {
    write_file(path,"{\"a\":1}\n{\"a\":2.5,\"late\":\"2024-02-29\"}\n{\"a\":\"text\",\"late\":null}\n");
    must(infer(Format::JSONL),OK,error);assert(binding.columns.size()==2&&binding.columns[0].type==Type::VARCHAR&&binding.columns[1].type==Type::DATE&&binding.columns[1].nullable);
    open();next();assert(row[0].text=="1"&&row[1].is_null);
  } else if (mode == "utf8_prefix") {
    write_file(path,"ＩＤ,field\n1,valid\n");must(infer(),OK,error);assert(binding.columns[0].name=="ＩＤ");
    open();next();assert(row[0].text=="1");must(scanner.rescan(error),OK,error);next();assert(row[0].text=="1");
  } else if (mode == "depth") {
    write_file(path,"{\"a\":"+std::string(1000000,'[')+"0"+std::string(1000000,']')+"}\n");
    must(infer(Format::JSONL),INVALID,error);
    Binding copy;must(deserialize_binding(std::string(100000,'[')+"0"+std::string(100000,']'),copy,error),INVALID,error);
  } else if (mode == "json_empty") {
    write_file(path,"{}\n{\"a\":1}\n{}\n");must(infer(Format::JSONL),OK,error);
    assert(binding.columns.size()==1&&binding.columns[0].nullable);open();next();assert(row[0].is_null);next();assert(row[0].text=="1");next();assert(row[0].is_null);
  } else if (mode == "chunks") {
    const std::string longtext=std::string(65533,'x')+"\"\n中文"+std::string(65536,'y');
    std::string escaped;for(char c:longtext){escaped+=c;if(c=='"')escaped+='"';}
    write_file(path,"id,note\r\n1,\""+escaped+"\"\r\n2,last\n");must(infer(),OK,error);open();next();assert(row[1].text==longtext);next();assert(row[1].text=="last");
    must(scanner.rescan(error),OK,error);next();assert(row[1].text==longtext);
  } else if (mode == "bounded") {
    {std::ofstream f(path);f<<"id,payload\n";const std::string payload(128,'x');for(int i=0;i<1000000;++i)f<<i<<','<<payload<<'\n';}
    rlimit limit{96*1024*1024,96*1024*1024};assert(setrlimit(RLIMIT_AS,&limit)==0);
    must(infer(),OK,error);assert(binding.columns[0].type==Type::BIGINT&&binding.columns[1].max_length==128);open();next();assert(row[0].text=="0");
  } else if (mode == "cancel") {
    {std::ofstream f(path);f<<"a\n";for(int i=0;i<10000;++i)f<<i<<'\n';}
    int checks=0;
    const auto cancelled=[&](){return ++checks>=100 ? -4012 : 0;};
    must(infer_file(path,dir,Format::CSV,options,binding,error,cancelled),-4012,error);
    assert(checks==100&&!error.empty()&&binding.columns.empty());
  } else if (mode == "scan_cancel") {
    const std::string text(2*1024*1024,'x');write_file(path,"a\n"+text+"\n");must(infer(),OK,error);open();
    int checks=0;const auto check_status=[&](){return ++checks>=3 ? -4012 : 0;};
    must(scanner.next(row,error,check_status),-4012,error);assert(checks==3&&row.empty());
    must(scanner.rescan(error),OK,error);next();assert(row[0].text==text);
  } else if (mode == "proc_size") {
    options.header=false;struct stat st;assert(stat("/proc/version",&st)==0&&S_ISREG(st.st_mode)&&st.st_size==0);
    must(infer_file("/proc/version","/proc",Format::CSV,options,binding,error),CHANGED,error);assert(!error.empty());
  } else if (mode == "future_time") {
    write_file(path,"a\n1\n");timespec times[2]={{13569465600LL,123},{13569465600LL,456}};
    assert(utimensat(AT_FDCWD,path.c_str(),times,0)==0);must(infer(),OK,error);open();next();assert(row[0].text=="1");
  } else assert(false);
}
