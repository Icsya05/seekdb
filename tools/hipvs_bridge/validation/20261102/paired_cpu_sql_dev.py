"""Paired public SIFT SQL development benchmark; not an official evaluation."""
import argparse, hashlib, json, os, pathlib, signal, subprocess, threading, time
import numpy as np
import pymysql
p=argparse.ArgumentParser()
p.add_argument("variant",choices=["cpu"])
p.add_argument("--nq",type=int,default=4096)
p.add_argument("--dataset",choices=["sift","gaussian256"],default="sift")
a=p.parse_args()
root=pathlib.Path("/workspace/amd-full-dbece630")
work=pathlib.Path("/workspace/seekdb_hipvs_notebook")
libdir=pathlib.Path("/workspace/amd-baseline") if a.variant=="baseline" else (pathlib.Path("/workspace/amd-trial80") if a.variant=="trial80" else root)
binary=root/"seekdb/build_release/src/observer/seekdb"
out=work/("paired-"+a.dataset+"-"+a.variant)
runtime=root/("paired-runtime-"+a.dataset+"-"+a.variant)
if out.exists() or runtime.exists():
    raise RuntimeError("existing benchmark output/runtime; inspect instead of overwriting")
out.mkdir(); runtime.mkdir()
result={"variant":a.variant,"dataset":a.dataset,"official_score":False,"backend":"CPU VSAG HNSW","server_cxx_source":"same frozen V5 C++ sources","nq":a.nq,"cases":[]}
process=None; connection=None; done=threading.Event(); samples=[]

def connect():
    return pymysql.connect(host="127.0.0.1",port=2992,user="root",autocommit=True,local_infile=True,read_timeout=300,write_timeout=300,connect_timeout=5)
def execute(statement):
    with connection.cursor() as cursor:
        cursor.execute(statement)
        return cursor.fetchall()
def monitor():
    paths=list(pathlib.Path("/sys/class/drm").glob("card*/device/mem_info_vram_used"))
    while not done.wait(0.02):
        values=[]
        for path in paths:
            try: values.append(int(path.read_text()))
            except OSError: pass
        if values: samples.append(max(values))
try:
    try:
        occupied=connect(); occupied.close()
    except pymysql.Error:
        pass
    else:
        raise RuntimeError("benchmark port already in use")
    env=os.environ.copy()
    env["LD_LIBRARY_PATH"]=f"{libdir}:/opt/hipvs/lib:/opt/rocm/lib:"+env.get("LD_LIBRARY_PATH","")
    env.pop("OB_VSAG_USE_CUVS",None)
    with (out/"console.log").open("w") as log:
        process=subprocess.Popen([str(binary),"--nodaemon","--port","2992","--data-dir",str(runtime/"store"),"--base-dir",str(runtime),"--parameter","__min_full_resource_pool_memory=2147483648","--parameter","memory_budget=8G","--parameter","datafile_size=8G","--parameter","cpu_count=24","--parameter","local_ip=127.0.0.1"],stdout=log,stderr=subprocess.STDOUT,env=env)
    for _ in range(180):
        if process.poll() is not None: raise RuntimeError(f"server exit {process.returncode}")
        try:
            connection=connect(); break
        except pymysql.Error: time.sleep(1)
    if connection is None: raise TimeoutError("SQL ready timeout")
    maps=pathlib.Path(f"/proc/{process.pid}/maps").read_text()
    actual=[line for line in maps.splitlines() if "libseekdb_cuvs_bridge.so" in line]
    (out/"bridge-maps.txt").write_text("\n".join(actual))
    if not actual or not all(str(libdir/"libseekdb_cuvs_bridge.so") in line for line in actual):
        raise AssertionError("unexpected loaded bridge variant")
    result["bridge_sha256"]=hashlib.file_digest((libdir/"libseekdb_cuvs_bridge.so").open("rb"),"sha256").hexdigest()
    execute("set ob_query_timeout=300000000")
    execute("alter system set ob_vector_memory_limit_percentage=30")
    execute("create database amd_sift")
    execute("use amd_sift")
    dim=128 if a.dataset=="sift" else 256
    execute(f"create table items(id bigint primary key,v vector({dim}))")
    data=work/("sift250k-dev" if a.dataset=="sift" else "gaussian256-dev")
    execute(f"load data local infile {json.dumps(str(data/'base.tsv'))} into table items fields terminated by '\\t' lines terminated by '\\n' (id,v)")
    if execute("select count(*) from items")[0][0]!=250000: raise AssertionError("load count")
    query=np.load(data/"query.npy")[:a.nq]
    truth=np.load(data/"truth100.npy")[:a.nq]
    def statement(q,k):
        vector="["+",".join(format(float(x),".9g") for x in q)+"]"
        return f"select id from items order by l2_distance(v,{json.dumps(vector)}) approximate limit {k}"
    thread=threading.Thread(target=monitor,daemon=True); thread.start()
    begin=time.perf_counter()
    execute("create vector index idx on items(v) with(distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=200)")
    first=execute(statement(query[0],10))
    result["build_through_first_query_seconds"]=time.perf_counter()-begin
    if len(first)!=10: raise AssertionError("first query result size")
    for k in [10,100]:
        outputs=[]; begin=time.perf_counter()
        for qi,q in enumerate(query):
            rows=execute(statement(q,k)); ids=[int(row[0]) for row in rows]
            if len(ids)!=k or len(set(ids))!=k or any(i<0 or i>=250000 for i in ids):
                raise AssertionError(f"invalid result IDs: k={k}, query={qi}")
            outputs.append(ids)
        elapsed=time.perf_counter()-begin
        recall=sum(len(set(ids)&set(truth[i,:k])) for i,ids in enumerate(outputs))/(len(query)*k)
        np.save(out/f"ids-top{k}.npy",np.asarray(outputs,dtype=np.int64))
        case={"topk":k,"qps":len(query)/elapsed,"seconds":elapsed,"recall":recall,"result_integrity":True}
        result["cases"].append(case); print(json.dumps(case),flush=True)
    result["status"]="PASS_DIAGNOSTIC"
except Exception as error:
    result["status"]="FAIL"; result["error"]=repr(error); print(repr(error),flush=True)
finally:
    done.set()
    result["global_device_vram_peak_bytes"]=max(samples) if samples else None
    result["vram_sampler"]="20ms DRM global allocated VRAM; may miss short transients; not official sampler"
    if connection is not None: connection.close()
    if process is not None and process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try: process.wait(timeout=60)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
    (out/"summary.json").write_text(json.dumps(result,indent=2))
    print(json.dumps(result),flush=True)