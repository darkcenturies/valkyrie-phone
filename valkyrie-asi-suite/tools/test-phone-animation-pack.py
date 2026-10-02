"""Independently validate the shipped ANP3 and both icon inventories."""
from pathlib import Path
import math
import struct
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
print('PASS: 18 valid upper-body clips, closed loops, distinct selfie/shutter poses and both icon sets.')
