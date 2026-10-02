"""Bake real world3d atlases into top-down D3D9 radar tiles."""
import io
import argparse, json, math, os, struct, subprocess, tempfile
from concurrent.futures import ThreadPoolExecutor
from PIL import Image, ImageDraw

COMP={5120:("b",1),5121:("B",1),5122:("h",2),5123:("H",2),5125:("I",4),5126:("f",4)}
COUNT={"SCALAR":1,"VEC2":2,"VEC3":3,"VEC4":4}; TILE=512.; SIZE=256
def glb(path):
 d=open(path,"rb").read(); n,_=struct.unpack_from("<II",d,12); doc=json.loads(d[20:20+n].decode().rstrip(" \0")); at=20+n; blen,_=struct.unpack_from("<II",d,at); return doc,memoryview(d)[at+8:at+8+blen]
def acc(doc,blob,index):
 a=doc["accessors"][index];v=doc["bufferViews"][a["bufferView"]];code,size=COMP[a["componentType"]];num=COUNT[a["type"]];stride=v.get("byteStride",size*num);start=v.get("byteOffset",0)+a.get("byteOffset",0);fmt="<"+code*num
 return [struct.unpack_from(fmt,blob,start+i*stride) for i in range(a["count"])],a
def atlas(original,ktx,tmp):
 doc,blob=glb(original); view=doc["bufferViews"][doc["images"][0]["bufferView"]];start=view.get("byteOffset",0)
 data=blob[start:start+view["byteLength"]]
 # Tiles that have been through world3d-pack carry the sheet as KTX2, which
 # needs transcoding before PIL can see it. A tile straight out of the bake
 # carries an ordinary PNG, and the radar set is built that way - the pack
 # step exists to make tiles small enough to stream to a browser, which is
 # not something the radar needs. Read whichever it turns out to be.
 if data[:4]!=bytes((0xAB,0x4B,0x54,0x58)):return Image.open(io.BytesIO(data)).convert("RGBA")
 kp=os.path.join(tmp,"a.ktx2");pp=os.path.join(tmp,"a.png");open(kp,"wb").write(data)
 subprocess.run([ktx,"extract","--transcode","rgba8","--level","0",kp,pp],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 return Image.open(pp).convert("RGBA")
def bake(original,stripped,dst,tx,ty,cell,ktx):
 with tempfile.TemporaryDirectory(prefix="sprp-radar-") as tmp: tex=atlas(original,ktx,tmp)
 doc,blob=glb(stripped); tris=[]; ox,oy=tx*TILE,ty*TILE; tw,th=tex.size; pix=tex.load()
 for node in doc.get("nodes",[]):
  if "mesh" not in node:continue
  tr=node.get("translation",[0,0,0]);sc=node.get("scale",[1,1,1])
  for prim in doc["meshes"][node["mesh"]]["primitives"]:
   at=prim["attributes"]; pos,_=acc(doc,blob,at["POSITION"]);uv0,_=acc(doc,blob,at["TEXCOORD_0"]);uv1,_=acc(doc,blob,at["TEXCOORD_1"]);cols,ca=acc(doc,blob,at["COLOR_0"]);ind,_=acc(doc,blob,prim["indices"]); ind=[x[0] for x in ind]
   for n in range(0,len(ind)-2,3):
    ids=ind[n:n+3]; vs=[(pos[i][0]*sc[0]+tr[0],pos[i][1]*sc[1]+tr[1],pos[i][2]*sc[2]+tr[2]) for i in ids];pts=[((v[0]-ox)*SIZE/TILE,(oy+TILE-v[1])*SIZE/TILE) for v in vs]
    area=abs((pts[1][0]-pts[0][0])*(pts[2][1]-pts[0][1])-(pts[1][1]-pts[0][1])*(pts[2][0]-pts[0][0]));
    if area<.02:continue
    u0=[sum(uv0[i][q] for i in ids)/3 for q in (0,1)];u1=[sum(uv1[i][q] for i in ids)/3 for q in (0,1)];u=u1[0]+(u0[0]-math.floor(u0[0]))*cell;v=u1[1]+(u0[1]-math.floor(u0[1]))*cell
    r,g,b,a=pix[max(0,min(tw-1,int(u*tw))),max(0,min(th-1,int((1-v)*th)))]; den=255 if ca.get("normalized") else 1;ao=sum(cols[i][0]/den for i in ids)/3
    # Alpha is a compact signed-ish height channel for the runtime's shallow
    # oblique projection: 32 represents sea level and each step is one unit.
    height=max(0,min(255,int(round(sum(vv[2] for vv in vs)/3+32))))
    col=(int(r*ao),int(g*ao),int(b*ao),height)
    if a>=96:tris.append((sum(vv[2] for vv in vs)/3,pts,col))
 tris.sort(key=lambda t:t[0]);im=Image.new("RGBA",(SIZE,SIZE),(20,28,35,32));draw=ImageDraw.Draw(im)
 for _,pts,col in tris:draw.polygon(pts,fill=col)
 with open(dst,"wb") as f:
  f.write(struct.pack("<4sII",b"R3M1",SIZE,SIZE))
  for r,g,b,a in im.getdata():f.write(struct.pack("<I",(a<<24)|(r<<16)|(g<<8)|b))
def main():
 ap=argparse.ArgumentParser();ap.add_argument("original");ap.add_argument("stripped");ap.add_argument("output");ap.add_argument("--range",default="-100,100,-100,100");ap.add_argument("--jobs",type=int,default=6);ap.add_argument("--ktx",default=r"C:\Program Files\KTX-Software\bin\ktx.exe");a=ap.parse_args();x0,x1,y0,y1=map(int,a.range.split(','));meta=json.load(open(os.path.join(a.original,"tiles.json")));cells={(t["x"],t["y"]):t.get("cell",1) for t in meta["tiles"]};work=[]
 for (x,y),cell in cells.items():
  if not(x0<=x<=x1 and y0<=y<=y1):continue
  name=f"{x}_{y}.glb";op=os.path.join(a.original,name);sp=os.path.join(a.stripped,name)
  if os.path.exists(op) and os.path.exists(sp):work.append((op,sp,os.path.join(a.output,f"{x}_{y}.r3m"),x,y,cell,a.ktx))
 done=[0]
 def run(item):
  bake(*item);done[0]+=1
  if done[0]%25==0 or done[0]==len(work):print(f"textured {done[0]}/{len(work)}",flush=True)
 with ThreadPoolExecutor(max_workers=max(1,a.jobs)) as pool:list(pool.map(run,work))
 print(f"textured {done[0]} tiles")
if __name__=="__main__":main()
