"""Author phone-specific upper-body clips for SA's standard ped skeleton.

All motion targets and timing below are authored here; no stock animation
keyframes are copied. Coordinates: left, up, back in the SA skin bind frame.
ANP3 layout follows gta-reversed AnimManager/AnimSequenceFrames (uncompressed
rotation tracks; absolute timestamps on disk). Legs/root are deliberately absent.
"""
import argparse
import math
from pathlib import Path
import struct
import numpy as np

HERE = Path(__file__).resolve().parent
OUTPUT = HERE / '../assets/animations/valkyrie-phone.ifp'
BLOCK = 'vp_phone'
UP = np.array([0., 1., 0.])
BACK = np.array([0., 0., 1.])
LEFT = np.array([1., 0., 0.])
# Standard anatomical axes, rather than sampled game animation poses.
NECK = np.column_stack((UP, BACK, LEFT))
CLAV = {31: np.column_stack((LEFT, -BACK, UP)),
        21: np.column_stack((-LEFT, -BACK, -UP))}
BONES = (4, 5, 31, 32, 33, 34, 35, 36, 21, 22, 23, 24, 25, 26)
POSES = {
    'rest': ((.34, .02, -.04), (-.34, .02, -.04), 0),
    'hold': ((.31, .04, -.09), (-.34, .02, -.04), 0),
    'use': ((.12, .34, -.29), (-.24, .22, -.16), 10),
    'type': ((.12, .34, -.29), (.01, .34, -.27), 10),
    'camera': ((.13, .65, -.44), (-.02, .55, -.38), 0),
    'selfie': ((.28, .67, -.49), (-.30, .12, -.08), -3),
    'call': ((.15, .72, -.035), (-.34, .02, -.04), -4),
}

def unit(v):
    return v / np.linalg.norm(v)

def quaternion(m):
    # Stable matrix conversion, including 180-degree clavicle rotations.
    t = float(np.trace(m))
    if t > 0:
        s = math.sqrt(t + 1) * 2
        q = np.array([(m[2,1]-m[1,2])/s, (m[0,2]-m[2,0])/s,
                      (m[1,0]-m[0,1])/s, s/4])
    else:
        i = int(np.argmax(np.diag(m))); j=(i+1)%3; k=(i+2)%3
        s=math.sqrt(max(0.,1+m[i,i]-m[j,j]-m[k,k]))*2
        q=np.zeros(4); q[i]=s/4; q[j]=(m[j,i]+m[i,j])/s
        q[k]=(m[k,i]+m[i,k])/s; q[3]=(m[k,j]-m[j,k])/s
    return unit(q)

def rotation_z(degrees):
    a=math.radians(degrees); c=math.cos(a); s=math.sin(a)
    return np.array([[c,-s,0],[s,c,0],[0,0,1.]])

def arm(target, left):
    shoulder=np.array([.19 if left else -.19,.52,0.])
    delta=target-shoulder; distance=float(np.linalg.norm(delta))
    direction=unit(delta); distance=min(.579,max(.025,distance))
    wrist=shoulder+direction*distance
    # Elbow bends laterally and downward, away from the phone and chest.
    pole=np.array([1. if left else -1.,-.8,.15])
    bend=unit(pole-direction*np.dot(pole,direction))
    along=(.30**2-.28**2+distance**2)/(2*distance)
    elbow=shoulder+direction*along+bend*math.sqrt(max(0.,.30**2-along**2))
    def basis(axis):
        x=unit(axis); z=unit(np.cross(x,-BACK)); y=np.cross(z,x)
        return np.column_stack((x,y,z))
    upper=basis(elbow-shoulder); fore=basis(wrist-elbow)
    # Grip keeps the model upright with the configured 180-degree turn/flip.
    hand=np.column_stack((LEFT if left else -LEFT,BACK,-UP if left else UP))
    clav=31 if left else 21; upper_id=clav+1
    rotations={clav:quaternion(NECK.T@CLAV[clav]),
               upper_id:quaternion(CLAV[clav].T@upper),
               clav+2:quaternion(upper.T@fore),
               clav+3:quaternion(fore.T@hand),
               clav+4:quaternion(rotation_z(30 if left else -18)),
               clav+5:quaternion(rotation_z(48 if left else -30))}
    return rotations,(shoulder,elbow,wrist)

def pose(a,b,progress,t,activity=''):
    smooth=progress*progress*(3-2*progress)
    l0,r0,h0=POSES[a]; l1,r1,h1=POSES[b]
    l=np.array(l0)+(np.array(l1)-l0)*smooth
    r=np.array(r0)+(np.array(r1)-r0)*smooth
    h=h0+(h1-h0)*smooth
    # Closed cycles: no random offsets, no wrist jumps at loop boundaries.
    if activity=='type': r[2]+=.012*(1-math.cos(2*math.pi*t/.72))
    if activity=='idle': l[1]+=.003*math.sin(2*math.pi*t/2.4)
    if activity=='photo': l[2]-=.004*math.sin(math.pi*progress)**2
    rotations,lp=arm(l,True); right,rp=arm(r,False); rotations.update(right)
    rotations[4]=quaternion(rotation_z(-h*.25))
    rotations[5]=quaternion(rotation_z(-h*.75))
    if activity=='type': rotations[25]=quaternion(rotation_z(-10-12*math.sin(2*math.pi*t/.72)**2))
    if activity=='photo': rotations[35]=quaternion(rotation_z(30+8*math.sin(math.pi*progress)**2))
    return rotations,(lp,rp)

# Every action has its own authored clip; selfies/shutters do not borrow the
# photography stance or replay the entry after each photograph.
CLIPS = (
    ('vp_takeout','rest','use',.72,''),
    ('vp_takeout_low','rest','hold',.5,''),
    ('vp_use','use','use',2.4,'idle'),
    ('vp_hold','hold','hold',2.4,'idle'),
    ('vp_type','type','type',.72,'type'),
    ('vp_putaway','use','rest',.65,''),
    ('vp_putaway_low','hold','rest',.45,''),
    ('vp_camera','use','camera',.65,''),
    ('vp_selfie','use','selfie',.82,''),
    ('vp_photo','camera','camera',.28,'photo'),
    ('vp_selfie_shot','selfie','selfie',.28,'photo'),
    ('vp_camera_out','camera','use',.56,''),
    ('vp_selfie_out','selfie','use',.65,''),
    ('vp_to_selfie','camera','selfie',.65,''),
    ('vp_to_camera','selfie','camera',.65,''),
    ('vp_call_in','use','call',.7,''),
    ('vp_call','call','call',2.4,'idle'),
    ('vp_call_out','call','use',.65,''),
)

def name24(s): return s.encode('ascii').ljust(24,b'\0')

def generate():
    payload=bytearray(name24(BLOCK)+struct.pack('<I',len(CLIPS)))
    for name,a,b,seconds,activity in CLIPS:
        count=round(seconds*30)+1
        times=[i*seconds/(count-1) for i in range(count)]
        frames=[pose(a,b,i/(count-1),t,activity)[0] for i,t in enumerate(times)]
        payload+=name24(name)+struct.pack('<III',len(BONES),len(BONES)*count*20,0)
        for bone in BONES:
            payload+=name24(f'bone_{bone}')+struct.pack('<III',1,count,bone)
            previous=None
            for t,f in zip(times,frames):
                q=f[bone]
                if previous is not None and np.dot(q,previous)<0: q=-q
                previous=q
                payload+=struct.pack('<5f',*q,t)
    return b'ANP3'+struct.pack('<I',len(payload))+payload

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check',action='store_true'); args=parser.parse_args()
    data=generate()
    if args.check:
        if OUTPUT.read_bytes()!=data: raise SystemExit('Regenerate the authored phone animation pack.')
    else:
        OUTPUT.parent.mkdir(parents=True,exist_ok=True); OUTPUT.write_bytes(data)
    print(f'{len(CLIPS)} authored phone clips; {len(data)} bytes; no root/leg tracks.')

if __name__=='__main__': main()
