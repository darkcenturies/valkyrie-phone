"""Pack textured world3d meshes for the isolated in-game 3D radar."""
import argparse, json, math, os, runpy, struct, tempfile
from concurrent.futures import ProcessPoolExecutor, as_completed
from PIL import Image

lib=runpy.run_path(os.path.join(os.path.dirname(__file__),"world3d-radar-texture.py"))
glb,acc,atlas=lib["glb"],lib["acc"],lib["atlas"]
# The atlas is sized per tile, not fixed.
#
# Every material in a tile gets an equal cell in a uniform grid, and "cell" (in
# tiles.json, in UV) says how big that cell is - so a tile's material count
# alone decides it. world3d already sizes each source sheet to give a constant
# 64 to 128 texels per cell whatever that count is, which is exactly right.
#
# Resizing every sheet to a fixed 512 threw that away. A dense downtown tile
# ships a 2800px sheet with ~1936 materials; squashed to 512 each material was
# left with 11.7 texels, while a near-empty tile with four materials was
# UPSCALED from 288 to 512 and sat at 227 texels for nothing. Measured across
# the set that is a 19x spread in texel density, worst exactly where there is
# most to look at, and two neighbouring tiles could differ by 10x - which is
# why one building could be crisp on one side of a tile seam and a blurred LOD
# smear on the other.
#
# Sizing from the tile's own cell instead holds the density roughly constant
# and spends the memory where there is detail to keep: sparse tiles get
# SMALLER sheets than before, dense ones larger.
TEXELS_PER_CELL=64
# The runtime refuses an atlas over 2048 (see Load in radar3d.cpp), so the
# densest tiles land near 47 texels rather than the full 64. The floor keeps a
# nearly empty tile from producing a sheet too small to hold the safe block.
ATLAS_MIN,ATLAS_MAX=128,2048
def atlas_size(cell):
 want=TEXELS_PER_CELL/max(cell,1e-6)
 power=1<<max(0,int(round(math.log2(max(want,1.0)))))
 return max(ATLAS_MIN,min(ATLAS_MAX,power))
SAFE_COLOUR=(140,140,140,255)
# 128-unit cells keep the camera corridor selective without turning each
# visible tile into hundreds of draw calls on the per-frame isolated render.
GROUP=128.0
def one(original,stripped,out,x,y,cell,ktx,geometry_only=False):
 # This tile's atlas size, and the safe UV that goes with it. Both follow from
 # `cell`; see atlas_size above.
 ATLAS=atlas_size(cell)
 SAFE_UV=(1.5/ATLAS,1.5/ATLAS)
 # Masked primitives are dropped, and props are dropped whole in the bake.
 #
 # Both, because neither is enough on its own.
 #
 # Dropping the masked pass alone is what produced traffic light heads with no
 # pole and branches with no tree: by the time a tile gets here every object in
 # it is merged into one mesh per material, so a pass is all there is to select
 # on, and most of these props straddle both passes. That is fixed upstream,
 # where world3d-bake.py drops matching objects whole - see PROPS there.
 #
 # But a name list only removes what it can name, and vegetation will not be
 # named: this world's foliage includes pinebg_hi, sm_redwoodgrp and
 # ulv_flowerpatch02, while "wood" as a pattern catches woodenbox and
 # sw_woodhaus04. Dropping the masked pass here removes the rest of the canopy
 # regardless of what it is called.
 #
 # What is left half-removed is an unnamed prop with an opaque part. That is a
 # smaller set than either filter leaves on its own, and the real fix is to
 # drop objects by their alpha share at bake time, where the whole object is
 # still addressable - the bake already knows which textures carry alpha,
 # since it sets alphaMode on them.
 source_doc,_=glb(original)
 source_materials=source_doc.get("materials",[])
 source_primitives=[p for m in source_doc.get("meshes",[]) for p in m.get("primitives",[])]
 doc,blob=glb(stripped); vertices=[];indices=[];primitive_index=0
 for node in doc.get("nodes",[]):
  if "mesh" not in node:continue
  tr=node.get("translation",[0,0,0]);sc=node.get("scale",[1,1,1])
  for prim in doc["meshes"][node["mesh"]]["primitives"]:
   source_prim=source_primitives[primitive_index] if primitive_index<len(source_primitives) else {}
   primitive_index+=1
   material_index=source_prim.get("material",-1)
   material_name=(source_materials[material_index].get("name","") if 0<=material_index<len(source_materials) else "").lower()
   if material_name=="masked":continue
   at=prim["attributes"];pos,_=acc(doc,blob,at["POSITION"]);u0,_=acc(doc,blob,at["TEXCOORD_0"]);u1,_=acc(doc,blob,at["TEXCOORD_1"]);cols,ca=acc(doc,blob,at["COLOR_0"]);ind,_=acc(doc,blob,prim["indices"]);den=255 if ca.get("normalized") else 1
   raw=[]
   for vi,(p,a,b,c) in enumerate(zip(pos,u0,u1,cols)):
    # Keep all three channels.
    #
    # This used to take c[0] alone and write it back as grey. COLOR_0 is not a
    # shade: world3d-bake puts the material's own colour there and then applies
    # the baked sunlight on top of it, so the red channel is only the whole
    # story for a surface that happens to be grey. Everything else lost its
    # colour here - which mattered most for materials with no texture, because
    # the atlas's white cell plus a colour is exactly how those are meant to be
    # drawn, and white times grey is not it.
    rgb=[max(0,min(255,c[q]/den*255)) for q in (0,1,2)]
    raw.append({"x":p[0]*sc[0]+tr[0],"y":p[1]*sc[1]+tr[1],"z":p[2]*sc[2]+tr[2],
                "r":rgb[0],"g":rgb[1],"b":rgb[2],
                "au":a[0],"av":a[1],"bu":b[0],"bv":b[1],"_src":vi})
   # Road and ground textures tile many times across world space, so a
   # vertex's own TEXCOORD_0 is routinely something like 12.7 before being
   # wrapped into its 0..1 atlas cell. Wrapping each vertex independently is
   # correct at that single point, but a triangle whose corners straddle a
   # tile boundary - one at 0.97, its neighbour at 5.03 (independently wraps
   # to 0.03) - then gets linearly interpolated across the wrapped gap
   # instead of the real 1.0-crossing distance: the rasterizer draws that as
   # a flat smear covering most of the atlas cell.
   #
   # Two earlier attempts at this both assumed a triangle crosses at most
   # one tile boundary - dropping such triangles lost most of the ground
   # plane (real ground triangles routinely span more than half a tile
   # without crossing one), and rewrapping against a single shared reference
   # produced garbage wherever a triangle actually spans *multiple* tiles
   # (this world's road/ground triangles are coarse enough that the texture
   # repeats several times across one triangle - common). Neither a drop
   # threshold nor a single reference wrap can be correct for that case: no
   # single atlas cell can represent a texture that repeats several times
   # across one triangle.
   #
   # The actual fix is standard texture-atlas seam splitting: clip every
   # triangle against each integer U and V line its raw (pre-wrap) UV
   # bounding box crosses, so every resulting piece's raw UV footprint sits
   # entirely inside one tile. Only then does "wrap the whole piece by one
   # shared reference" become correct rather than approximate - because
   # after clipping there genuinely is only one tile for it to belong to.
   def lerp(v0,v1,t):
    out={k:(v0[k]+(v1[k]-v0[k])*t) for k in v0 if k!="_src"}
    out["_src"]=None  # a synthesised clip point, not one of the primitive's own vertices - not cacheable
    return out
   def clip(poly,key,boundary,keep_le):
    out=[]
    n=len(poly)
    for i in range(n):
     cur=poly[i];nxt=poly[(i+1)%n]
     cur_in=(cur[key]<=boundary) if keep_le else (cur[key]>=boundary)
     nxt_in=(nxt[key]<=boundary) if keep_le else (nxt[key]>=boundary)
     if cur_in:out.append(cur)
     if cur_in!=nxt_in:
      d=nxt[key]-cur[key]
      t=0.0 if abs(d)<1e-12 else (boundary-cur[key])/d
      out.append(lerp(cur,nxt,t))
    return out
   def split_axis(polys,key):
    result=[]
    for poly in polys:
     lo=math.floor(min(v[key] for v in poly));hi=math.floor(max(v[key] for v in poly))
     if lo==hi:
      result.append(poly);continue
     remaining=poly
     for k in range(lo+1,hi+1):
      left=clip(remaining,key,k,True);right=clip(remaining,key,k,False)
      if len(left)>=3:result.append(left)
      remaining=right
     if len(remaining)>=3:result.append(remaining)
    return result
   # A vertex shared by several untouched (non-seam) triangles was, before
   # this cache, re-emitted fresh for every one of them - inflating even
   # tiles with no actual seams several-fold, since most of a tile's
   # vertices are shared by many triangles. Only a synthesised clip point
   # (born from lerp, _src is None) is genuinely one-off; a triangle's own
   # original corner wrapped against a given (ref_u, ref_v) always produces
   # the same output vertex, so cache and reuse it.
   wrap_cache={}
   def wrapped_vertex(v,ref_u,ref_v):
    key=(v["_src"],ref_u,ref_v) if v["_src"] is not None else None
    if key is not None and key in wrap_cache:return wrap_cache[key]
    nu=v["bu"]+(v["au"]-ref_u)*cell;nv=v["bv"]+(v["av"]-ref_v)*cell
    r,g,b=(max(0,min(255,int(round(v[k])))) for k in ("r","g","b"))
    colour=0xff000000|(r<<16)|(g<<8)|b
    vertices.append((v["x"],v["y"],v["z"],colour,nu,nv))
    idx=len(vertices)-1
    if key is not None:wrap_cache[key]=idx
    return idx
   # Exact clipping is correct for any span, but pure-Python polygon
   # clipping over every one of a tile's several hundred thousand triangles
   # does not fit either a shippable pack time or a shippable file size -
   # the full version of this ran toward 3.5 hours and 10GB+ for one world,
   # because this world's road/ground textures repeat often enough that
   # most ground triangles need splitting at least once, and the rare
   # triangle that repeats a texture many times over needs many pieces.
   #
   # Bound both: only clip triangles that cross at most SPLIT_CAP tile
   # boundaries per axis (the vast majority - one or two boundaries covers
   # ordinary seam crossings). A triangle beyond that cap is rare and
   # already an extreme case - repeating a texture many times across itself.
   # Wrapping such a triangle against a single reference tile (an earlier
   # version of this fix) does NOT bound the damage to "one tile's worth of
   # drift": a corner many tiles away from the reference lands far outside
   # [0,1] within that cell, and CLAMP addressing then samples whatever real
   # texel happens to be nearest the atlas edge in that direction - visibly
   # the same wrong-cell-sampling failure as the original bug, just rarer.
   # So a fallback triangle instead points at SAFE_UV, a small block the
   # atlas-writing step below fills with a fixed neutral colour - guaranteed
   # safe because it never depends on where in the atlas the real texture
   # happens to sit.
   SPLIT_CAP=10
   local_ind=[i[0] for i in ind]
   for n in range(0,len(local_ind)-2,3):
    tri=[raw[t] for t in local_ind[n:n+3]]
    us=[v["au"] for v in tri];vs=[v["av"] for v in tri]
    span_u=math.floor(max(us))-math.floor(min(us));span_v=math.floor(max(vs))-math.floor(min(vs))
    if span_u<=SPLIT_CAP and span_v<=SPLIT_CAP:
     pieces=split_axis([tri],"au")
     pieces=split_axis(pieces,"av")
     for poly in pieces:
      cu=sum(v["au"] for v in poly)/len(poly);cv=sum(v["av"] for v in poly)/len(poly)
      ref_u,ref_v=math.floor(cu),math.floor(cv)
      out_idx=[wrapped_vertex(v,ref_u,ref_v) for v in poly]
      for i in range(1,len(out_idx)-1):
       indices.extend((out_idx[0],out_idx[i],out_idx[i+1]))
    else:
     out_idx=[]
     for v in tri:
      # White, so the safe block's own neutral colour is what comes through.
      # Black here meant these triangles rendered black the moment the vertex
      # colour started being used at all.
      vertices.append((v["x"],v["y"],v["z"],0xffffffff,SAFE_UV[0],SAFE_UV[1]))
      out_idx.append(len(vertices)-1)
     indices.extend(out_idx)
 # Weld UV-split vertices by position, then find connected geometry objects.
 # Spatial cells made unrelated trees fade with a blocking building and left
 # the building's faces in adjacent cells opaque. Object groups keep all
 # connected faces under one opacity value.
 parent=list(range(len(vertices)))
 def find(q):
  while parent[q]!=q:
   parent[q]=parent[parent[q]];q=parent[q]
  return q
 def union(a,b):
  a,b=find(a),find(b)
  if a!=b:parent[b]=a
 welded={}
 for i,v in enumerate(vertices):
  key=(round(v[0]*20),round(v[1]*20),round(v[2]*20))
  if key in welded:union(i,welded[key])
  else:welded[key]=i
 for n in range(0,len(indices)-2,3):
  a,b,c=indices[n:n+3];union(a,b);union(a,c)
 components={}
 for n in range(0,len(indices)-2,3):
  tri=indices[n:n+3];components.setdefault(find(tri[0]),[]).extend(tri)

 # Classify complete connected components first. Roofs and terraces are often
 # exported as separate, entirely horizontal components, so looking only for
 # a vertical triangle inside the component inevitably labels them as ground.
 # Retain the vertical building shells, then associate elevated horizontal
 # components with the footprint of those shells in a second pass.
 classified=[];building_shells=[]
 for root,ind in components.items():
  used=[vertices[q] for q in ind]
  minx,miny,minz=min(q[0] for q in used),min(q[1] for q in used),min(q[2] for q in used)
  maxx,maxy,maxz=max(q[0] for q in used),max(q[1] for q in used),max(q[2] for q in used)
  component_has_vertical=False
  for n in range(0,len(ind)-2,3):
   a,b,c=(vertices[ind[n+i]] for i in range(3))
   ux,uy,uz=b[0]-a[0],b[1]-a[1],b[2]-a[2];vx,vy,vz=c[0]-a[0],c[1]-a[1],c[2]-a[2]
   nx,ny,nz=uy*vz-uz*vy,uz*vx-ux*vz,ux*vy-uy*vx
   mag=max(1e-8,math.sqrt(nx*nx+ny*ny+nz*nz))
   if abs(nz)/mag<0.72 and max(a[2],b[2],c[2])-min(a[2],b[2],c[2])>.5:
    component_has_vertical=True;break
  # Remove only genuinely narrow vertical components. Roads can share vertices
  # with curbs, so a component is a complete fadeable building only when it
  # has vertical structure, meaningful height and building-sized bounds. That
  # includes its horizontal roof without turning a city-scale road component
  # into an occluder.
  width,depth,height=maxx-minx,maxy-miny,maxz-minz
  if component_has_vertical and width<=4.0 and depth<=4.0 and height>=3.5:continue
  building=component_has_vertical and height>=3.5 and max(width,depth)<=256.0
  item=(root,ind,minx,miny,minz,maxx,maxy,maxz,component_has_vertical,building)
  classified.append(item)
  if building:building_shells.append((root,minx,miny,minz,maxx,maxy,maxz))

 ground={};objects={}
 for root,ind,minx,miny,minz,maxx,maxy,maxz,component_has_vertical,building in classified:
  width,depth,height=maxx-minx,maxy-miny,maxz-minz
  building_id=root if building else None
  # A separate slab belongs to a building when a meaningful part of its XY
  # footprint overlaps a vertical shell and it is elevated above that shell's
  # base. This catches detached roofs, balconies and terraces while excluding
  # roads/terrain that merely occupy the same 128-unit packing cell.
  if not building and height<=3.5 and max(width,depth)<=256.0:
   area=max(.01,width*depth)
   for shell_id,sx0,sy0,sz0,sx1,sy1,sz1 in building_shells:
    overlap=max(0,min(maxx,sx1)-max(minx,sx0))*max(0,min(maxy,sy1)-max(miny,sy0))
    if overlap/area>=.35 and minz>=sz0+2.0 and minz<=sz1+3.0:
     building=True;building_id=shell_id;break
  for n in range(0,len(ind)-2,3):
   tri=ind[n:n+3];a,b,c=(vertices[q] for q in tri)
   ux,uy,uz=b[0]-a[0],b[1]-a[1],b[2]-a[2];vx,vy,vz=c[0]-a[0],c[1]-a[1],c[2]-a[2]
   nx,ny,nz=uy*vz-uz*vy,uz*vx-ux*vz,ux*vy-uy*vx
   mag=max(1e-8,math.sqrt(nx*nx+ny*ny+nz*nz))
   vertical=abs(nz)/mag<0.72 and max(a[2],b[2],c[2])-min(a[2],b[2],c[2])>.5
   cx=(a[0]+b[0]+c[0])/3;cy=(a[1]+b[1]+c[1])/3
   # One opacity group per connected building, including detached slabs that
   # were associated with its shell above. Spatially regrouping these triangles
   # made a ray hide the wall it crossed while leaving that same model's roof
   # visible when the camera sat inside/behind it.
   if building or vertical:objects.setdefault(building_id if building_id is not None else root,[]).extend(tri)
   else:ground.setdefault((math.floor(cx/GROUP),math.floor(cy/GROUP)),[]).extend(tri)
 groups=[(0,ind) for ind in ground.values()]+[(1,ind) for ind in objects.values()]
 os.makedirs(out,exist_ok=True);stem=f"{x}_{y}"
 with open(os.path.join(out,stem+".r3g"),"wb") as f:
  f.write(struct.pack("<4sII",b"R3G2",len(vertices),len(groups)))
  for v in vertices:f.write(struct.pack("<fffIff",*v))
  for flag,ind in groups:
   used=[vertices[q] for q in ind]
   bounds=(min(q[0] for q in used),min(q[1] for q in used),min(q[2] for q in used),max(q[0] for q in used),max(q[1] for q in used),max(q[2] for q in used))
   f.write(struct.pack("<6fII",*bounds,flag,len(ind)))
   for i in ind:f.write(struct.pack("<I",i))
 if geometry_only:return
 with tempfile.TemporaryDirectory(prefix="sprp-radar3d-") as tmp:im=atlas(original,ktx,tmp)
 if im.size!=(ATLAS,ATLAS):im=im.resize((ATLAS,ATLAS),Image.Resampling.LANCZOS)
 # A 4x4 block, not one pixel, around SAFE_UV's target - linear filtering
 # samples up to a pixel's neighbours, so a single safe pixel could still
 # blend in whatever real texture sits just outside it.
 for py in range(4):
  for px in range(4):im.putpixel((px,py),SAFE_COLOUR)
 with open(os.path.join(out,stem+".r3a"),"wb") as f:
  f.write(struct.pack("<4sII",b"R3A1",ATLAS,ATLAS))
  for r,g,b,a in im.getdata():f.write(struct.pack("<I",(a<<24)|(r<<16)|(g<<8)|b))
def main():
 ap=argparse.ArgumentParser();ap.add_argument("original");ap.add_argument("stripped");ap.add_argument("output");ap.add_argument("--range",default="-100,100,-100,100");ap.add_argument("--jobs",type=int,default=4);ap.add_argument("--ktx",default=r"C:\Program Files\KTX-Software\bin\ktx.exe");ap.add_argument("--geometry-only",action="store_true");a=ap.parse_args();x0,x1,y0,y1=map(int,a.range.split(','));meta=json.load(open(os.path.join(a.original,"tiles.json")));work=[]
 for t in meta["tiles"]:
  x,y=t["x"],t["y"]
  if x0<=x<=x1 and y0<=y<=y1:
   name=f"{x}_{y}.glb";op=os.path.join(a.original,name);sp=os.path.join(a.stripped,name)
   if os.path.exists(op) and os.path.exists(sp):work.append((op,sp,a.output,x,y,t.get("cell",1),a.ktx,a.geometry_only))
 # ThreadPoolExecutor never actually parallelised this: it's pure-Python
 # geometry clipping, CPU-bound, and the GIL keeps every thread but one
 # blocked regardless of --jobs. A real process pool gets the intended
 # multi-core speedup, which is the difference between this pack finishing
 # in tens of minutes rather than hours across 1101 tiles.
 done=0;failed=[]
 with ProcessPoolExecutor(max_workers=max(1,a.jobs)) as pool:
  future_tile={pool.submit(one,*w):(w[3],w[4]) for w in work}
  for f in as_completed(future_tile):
   x,y=future_tile[f]
   try:f.result()
   except Exception as e:
    failed.append((x,y,e));print(f"packed3d FAILED {x}_{y}: {e}",flush=True);continue
   done+=1
   if done%20==0 or done==len(work):print(f"packed3d {done}/{len(work)}",flush=True)
 if failed:
  print(f"packed3d: {len(failed)} tile(s) failed: {[f'{x}_{y}' for x,y,_ in failed]}",flush=True)
  raise SystemExit(1)
if __name__=="__main__":main()
