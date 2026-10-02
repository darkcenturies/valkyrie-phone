"""Independently validate the shipped ANP3 and both icon inventories."""
from pathlib import Path
import math
import struct
import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
data=(ROOT/'valkyrie-phone/assets/animations/valkyrie-phone.ifp').read_bytes()
magic,size=struct.unpack_from('<4sI',data)
assert magic==b'ANP3' and size==len(data)-8
assert data[8:32].split(b'\0')[0]==b'vp_phone'
clips=struct.unpack_from('<I',data,32)[0]
assert clips==18
at=36; parsed={}
for _ in range(clips):
    name=data[at:at+24].split(b'\0')[0].decode('ascii'); at+=24
    sequences,frame_bytes,flags=struct.unpack_from('<III',data,at); at+=12
    assert sequences==14 and flags==0
    actual_bytes=0; tracks={}
    for _ in range(sequences):
        at+=24
        kind,count,bone=struct.unpack_from('<III',data,at); at+=12
        assert kind==1 and count>=2
        assert bone in {4,5,21,22,23,24,25,26,31,32,33,34,35,36}
        assert bone not in tracks
        frames=[struct.unpack_from('<5f',data,at+i*20) for i in range(count)]
        at+=count*20; actual_bytes+=count*20
        assert frames[0][4]==0 and .2<=frames[-1][4]<=3
        for i,f in enumerate(frames):
            assert all(math.isfinite(v) for v in f)
            assert abs(sum(v*v for v in f[:4])-1)<1e-5
            if i:
                assert f[4]>frames[i-1][4]
                assert sum(a*b for a,b in zip(f[:4],frames[i-1][:4]))>=0
        tracks[bone]=frames
    assert actual_bytes==frame_bytes
    assert name not in parsed
    parsed[name]=tracks
assert at==len(data)
for name in ('vp_use','vp_hold','vp_type','vp_call'):
    for frames in parsed[name].values():
        assert abs(sum(a*b for a,b in zip(frames[0][:4],frames[-1][:4])))>0.99999
# Taking a selfie must not move the arm back to the rear-camera stance.
for bone in (32,33,34):
    a=parsed['vp_selfie'][bone][-1][:4]; b=parsed['vp_selfie_shot'][bone][0][:4]
    assert abs(sum(x*y for x,y in zip(a,b)))>0.99999
assert parsed['vp_selfie'][32][-1][:4]!=parsed['vp_camera'][32][-1][:4]
# Decode the packed quaternions independently and propagate a standard ped rig.
# Animated SA axes: world right +X, forward +Y, up +Z. The neck's local
# X/Y/Z axes are up/forward/left after root/pelvis animation, unlike DFF bind axes.
def rotation(frame):
    x,y,z,w=frame[:4]
    return np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                     [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                     [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])
def hand_pose(name, side=31, frame=-1):
    tracks=parsed[name]
    neck=np.array([[0,0,-1],[0,1,0],[1,0,0]])@rotation(tracks[4][frame])
    pos=np.array([0.,0.,.55])+neck@np.array([0.,0.,.033 if side==31 else -.033])
    orient=neck@rotation(tracks[side][frame])
    for bone,length in ((side+1,.165),(side+2,.296),(side+3,.281)):
        pos+=orient@np.array([length,0.,0.]);orient=orient@rotation(tracks[bone][frame])
    # Same local turns as HandTurn=180,0,0 and HandFlip=1 around model Z.
    model=orient@np.diag([1.,-1.,-1.])@np.diag([-1.,-1.,1.])
    return pos,model,neck@rotation(tracks[5][frame])
for name in ('vp_use','vp_type','vp_camera','vp_selfie','vp_photo','vp_selfie_shot'):
    pos,model,head=hand_pose(name)
    assert pos[1]>.12, f'{name}: left hand behind the character: {pos}'
    assert model[2,2]>.9, f'{name}: handset upside down'
    assert -model[1,1]>.9, f'{name}: rear lens faces the character'
    if name in ('vp_use','vp_type'):
        assert head[2,1]<-.05, f'{name}: head looks up rather than down'
pos,model,_=hand_pose('vp_call')
assert pos[0]<-.10 and pos[2]>.68, 'Call must hold the phone beside the left ear'
assert model[0,1]>.9, 'Call screen must face the left ear'
left,_,_=hand_pose('vp_type')
right,model,_=hand_pose('vp_type',side=21)
orient=model@np.diag([-1.,1.,-1.])
tip=right+orient@np.array([.088,0.,0.])
orient=orient@rotation(parsed['vp_type'][25][-1])
tip+=orient@np.array([.062,0.,0.])
assert np.linalg.norm(tip-left)<.20, 'Typing fingers point away from the handset'
for name in ('vp_camera','vp_selfie','vp_call_in','vp_to_selfie','vp_to_camera'):
    count=len(parsed[name][32])
    for frame in range(count):
        pos,_,_=hand_pose(name,frame=frame)
        assert pos[1]>-.05, f'{name}: transition passes behind the torso'
art=ROOT/'valkyrie-phone/assets/generated'
legacy=list(art.glob('*_64.png'))
assert len(legacy)>=80
for file in legacy:
    assert Image.open(file).size==(64,64)
for name in ('app_camera','app_photos','app_maps','app_phone','app_text','app_contacts',
             'app_internet','app_games','app_clock','app_calculator','app_notes','app_weather',
             'app_stocks','app_radio','app_calendar','app_flashlight'):
    assert Image.open(art/(name+'.png')).size==(16,16)
    assert Image.open(art/(name+'_64.png')).size==(64,64)
print('PASS: 18 valid clips, forward hand positions, upright handset, ear-facing call grip, closed loops and both icon sets.')
