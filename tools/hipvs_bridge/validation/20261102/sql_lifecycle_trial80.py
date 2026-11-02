"""SQL lifecycle diagnostic for the freshly built candidate, not official scoring."""
import json, os, pathlib, signal, subprocess, time
import numpy as np
ROOT = pathlib.Path("/workspace/amd-full-dbece630")
BIN = ROOT / "seekdb/build_release/src/observer/seekdb"
OUT = pathlib.Path("/workspace/seekdb_hipvs_notebook/sql-lifecycle-trial80")
RUN = ROOT / "sql-lifecycle-trial80-runtime"
PORT = 2991
OUT.mkdir(exist_ok=True)
RUN.mkdir(exist_ok=True)
MYSQL = ["mysql", "--local-infile=1", "--connect-timeout=5", "--protocol=TCP", "-h127.0.0.1", f"-P{PORT}", "-uroot", "-ANB"]
results = {"kind": "diagnostic_only", "variant": "trial80", "official_score": False, "cases": []}
proc = None

def sql(statement, timeout=300):
    statement = "set ob_query_timeout=300000000; " + statement
    completed = subprocess.run(MYSQL + ["-e", statement], capture_output=True, text=True, timeout=timeout)
    with (OUT / "sql-transcript.log").open("a") as stream:
        stream.write(statement + "\n" + completed.stdout + completed.stderr + "\n")
    if completed.returncode:
        raise RuntimeError(completed.stderr)
    return completed.stdout.strip()

def start():
    global proc
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = f"/workspace/amd-trial80:/opt/hipvs/lib:/opt/rocm/lib:" + env.get("LD_LIBRARY_PATH", "")
    env.pop("OB_VSAG_USE_CUVS", None)
    logfile = (OUT / "observer-console.log").open("a")
    proc = subprocess.Popen([str(BIN), "--nodaemon", "--port", str(PORT), "--data-dir", str(RUN / "store"), "--base-dir", str(RUN), "--parameter", "__min_full_resource_pool_memory=2147483648", "--parameter", "memory_budget=4G", "--parameter", "datafile_size=4G", "--parameter", "cpu_count=24", "--parameter", "local_ip=127.0.0.1"], stdout=logfile, stderr=subprocess.STDOUT, env=env)
    logfile.close()
    for attempt in range(180):
        if proc.poll() is not None:
            raise RuntimeError(f"candidate server exited {proc.returncode}")
        ready = subprocess.run(MYSQL + ["-e", "select 1"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        if ready.returncode == 0:
            return
        time.sleep(1)
    raise TimeoutError("SQL readiness timeout")

def stop():
    global proc
    if proc is not None and proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    proc = None

def record(name, detail):
    results["cases"].append({"name": name, "status": "PASS", "detail": detail})
    print(name, detail, flush=True)

try:
    if not BIN.is_file():
        raise FileNotFoundError(BIN)
    if (RUN / "store").exists():
        raise RuntimeError("runtime already contains data: inspect instead of overwriting")
    probe = subprocess.run(MYSQL + ["-e", "select 1"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
    if probe.returncode == 0:
        raise RuntimeError("test port already in use")
    rng = np.random.default_rng(20261102)
    base = rng.normal(size=(10000,128)).astype(np.float32)
    base /= np.linalg.norm(base, axis=1, keepdims=True)
    def vector(row):
        return "[" + ",".join(format(float(x), ".8g") for x in row) + "]"
    with (OUT / "base.tsv").open("w") as stream:
        for i, row in enumerate(base):
            stream.write(f"{i}\t{vector(row)}\n")
    queries = [vector(base[i]) for i in [0,123,1001,7777,9999]]
    start()
    record("candidate_sql_ready", {"binary": str(BIN)})
    sql("alter system set ob_vector_memory_limit_percentage=30; create database amd_lifecycle;")
    for table, backend in [("gpu_items","cuvs"),("cpu_items","vsag")]:
        sql(f"use amd_lifecycle; create table {table}(id bigint primary key,v vector(128)); load data local infile {json.dumps(str(OUT / 'base.tsv'))} into table {table} fields terminated by '\\t' lines terminated by '\\n' (id,v);")
        count = int(sql(f"select count(*) from amd_lifecycle.{table}"))
        if count != 10000:
            raise AssertionError(f"unexpected load count {count}")
        begin = time.monotonic()
        sql(f"use amd_lifecycle; create vector index idx on {table}(v) with(distance=l2,type=hnsw,lib={backend},m=16,ef_construction=200,ef_search=200);")
        first = sql(f"select id from amd_lifecycle.{table} order by l2_distance(v,{json.dumps(queries[0])}) approximate limit 10")
        record(f"{backend}_offline_ddl_and_first_query", {"rows": count, "seconds": time.monotonic()-begin})
        if len(first.splitlines()) != 10:
            raise AssertionError("expected 10 neighbors")
    before = {}
    for table in ["gpu_items","cpu_items"]:
        for qi,q in enumerate(queries):
            statement = f"select id from amd_lifecycle.{table} order by l2_distance(v,{json.dumps(q)})"
            exact = sql(statement + " limit 10").splitlines()
            approx = sql(statement + " approximate limit 10").splitlines()
            recall = len(set(exact)&set(approx))/10
            if recall < 0.9 or len(set(approx)) != 10:
                raise AssertionError(f"{table} query {qi} recall {recall}")
            before[(table,qi)] = approx
    record("pre_restart_sql_recall", {"queries_per_backend":len(queries), "minimum_required":0.9})
    stop()
    start()
    for (table,qi),previous in before.items():
        current = sql(f"select id from amd_lifecycle.{table} order by l2_distance(v,{json.dumps(queries[qi])}) approximate limit 10").splitlines()
        if set(current) != set(previous):
            raise AssertionError(f"neighbors changed after restart: {table} {qi}")
    record("restart_sql_results", {"comparisons":len(before)})
    results["status"] = "PASS"
except Exception as error:
    results["status"] = "FAIL"
    results["error"] = repr(error)
    print(repr(error), flush=True)
finally:
    stop()
    (OUT / "summary.json").write_text(json.dumps(results, indent=2))