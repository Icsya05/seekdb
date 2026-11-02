"""Seeded 256D development data; never a surrogate proof of hidden-set quality."""
import json,pathlib,time
import numpy as np
out=pathlib.Path("/workspace/seekdb_hipvs_notebook/gaussian256-dev")
if out.exists(): raise RuntimeError("existing dataset: inspect instead of overwriting")
out.mkdir()
rng=np.random.default_rng(20261101+256)
base=rng.standard_normal((250000,256),dtype=np.float32)
query=rng.standard_normal((4096,256),dtype=np.float32)
np.save(out/"base.npy",base); np.save(out/"query.npy",query)
truth=np.empty((4096,100),dtype=np.int64)
x=base.astype(np.float64); norms=(x*x).sum(axis=1)
begin=time.monotonic()
for offset in range(0,len(query),32):
    q=query[offset:offset+32].astype(np.float64)
    d=(q*q).sum(axis=1)[:,None]+norms[None,:]-2*(q@x.T)
    ids=np.argpartition(d,99,axis=1)[:,:100]
    order=np.argsort(np.take_along_axis(d,ids,axis=1),axis=1,kind="stable")
    truth[offset:offset+len(q)]=np.take_along_axis(ids,order,axis=1)
np.save(out/"truth100.npy",truth)
with (out/"base.tsv").open("w") as stream:
    for i,row in enumerate(base):
        stream.write(str(i)+"\t["+",".join(format(float(v),".9g") for v in row)+"]\n")
metadata={"kind":"seeded Gaussian FP32 development proxy, not Hidden data", "seed":20261101+256,"base_shape":list(base.shape),"query_shape":list(query.shape),"truth":"exact L2 of FP32 input using FP64 CPU accumulation","seconds":time.monotonic()-begin,"hidden_pass_guaranteed":False}
(out/"metadata.json").write_text(json.dumps(metadata,indent=2)); print(json.dumps(metadata),flush=True)