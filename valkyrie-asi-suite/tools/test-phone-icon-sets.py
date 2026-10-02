from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[1]
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
print('PASS: both icon inventories retain their native dimensions.')
