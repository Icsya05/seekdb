"""Public SIFT development subset; does not claim official row/query selection."""
import hashlib, json, pathlib, time
import h5py
import numpy as np
root=pathlib.Path("/workspace/seekdb_hipvs_notebook")
source=root/"sift-mirror.hdf5"
expected="dd6f0a6ed6b7ebb8934680f861a33ed01ff33991eaee4fd60914d854a0ca5984"
digest=hashlib.file_digest(source.open("rb"),"sha256").hexdigest()
if digest!=expected:
    raise RuntimeError(f"public dataset hash mismatch: {digest}")
out=root/"sift250k-dev"
out.mkdir(exist_ok=True)
with h5py.File(source,"r") as data:
    print({key:data[key].shape for key in data},flush=True)
    base=np.asarray(data["train"][:250000],dtype=np.float32)
    query=np.asarray(data["test"][:4096],dtype=np.float32)
if base.shape!=(250000,128) or query.shape!=(4096,128):
    raise AssertionError("unexpected public SIFT shapes")
np.save(out/"base.npy",base)
np.save(out/"query.npy",query)
truth=np.empty((len(query),100),dtype=np.int64)
norms=(base*base).sum(axis=1)
begin=time.monotonic()
for offset in range(0,len(query),32):
    batch=query[offset:offset+32]
    distance=(batch*batch).sum(axis=1)[:,None]+norms[None,:]-2*(batch@base.T)
    ids=np.argpartition(distance,99,axis=1)[:,:100]
    selected=np.take_along_axis(distance,ids,axis=1)
    order=np.argsort(selected,axis=1,kind="stable")
    truth[offset:offset+len(batch)]=np.take_along_axis(ids,order,axis=1)
np.save(out/"truth100.npy",truth)
with (out/"base.tsv").open("w") as stream:
    for i,row in enumerate(base):
        stream.write(str(i)+"\t["+",".join(str(int(x)) for x in row)+"]\n")
metadata={"source":"https://huggingface.co/datasets/hhy3/ann-datasets/resolve/0bc1ed67fc2626bc6f110c8794a70572681b7395/sift-128-euclidean.hdf5","sha256":digest,"base_selection":"first 250000 train rows","query_selection":"first 4096 test rows","official_selection_verified":False,"truth":"recomputed exact FP32 L2 over selected 250K; boundary ties possible","truth_seconds":time.monotonic()-begin,"base_shape":list(base.shape),"query_shape":list(query.shape)}
(out/"metadata.json").write_text(json.dumps(metadata,indent=2))
print(json.dumps(metadata),flush=True)