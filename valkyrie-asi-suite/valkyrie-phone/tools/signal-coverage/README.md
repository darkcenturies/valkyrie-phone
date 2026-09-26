# Phone signal from the map's own masts

A study for giving the phone a signal that depends on where CJ is, carried by
the radio masts already standing on the Project Eagle map. Nothing here is
built into the phone yet.

## The masts

`find_masts.py` reads every IPL the game loads (268 text IPLs and 244 binary
stream IPLs in the IMG archives on the Project Eagle install this was run on)
and lists every placed model with mast, antenna, aerial, radio, transmit or
dish in its name.

`radarmast1_LAwN` (ID 13714), the model first considered, is placed once:
at (912, -801, 129) in the Vinewood hills. Its twin `radarmast1_LAwN01`
(ID 13758) is placed once beside it at (705, -917). None of Project Eagle's
added regions use either, so that model alone cannot carry a network.

Counting every radio structure, the map has eight usable sites. They are in
`masts.csv`, the input to `coverage.py`:

| Site | Model | Position |
| --- | --- | --- |
| Los Santos, Vinewood hills | radarmast1_LAwN (and _LAwN01, 2 x CE_radarmast3 nearby) | 912, -801, 129 |
| San Fierro, Missionary Hill | transmitter_sfs | -2502, -701, 227 |
| San Fierro, east | masts1_sfe | -1592, 696, 81 |
| Cheyenne | cyeradiotower | 2468, 5963, 135 |
| Carcer City | denton_mast (2) | 9882, 10198, 107 |
| Upstate | lcc_radiostat | 13744, 9095, 269 |
| Liberty City | ind_newradio | 15245, 8162, 20 |
| Vice City airport | mc_satdishlg_vcs | 13321, -7670, 32 |

Left out: power pylons (`pylon_big1_` and others, 250+ placements),
`ringmaster` (the Las Venturas circus sign), rooftop dishes and roof masts,
and the two dishes in interior 10.

## Coverage

`coverage.py` works out the signal in 100 x 100 unit cells, treating one unit
as one metre, at 900 MHz, the GSM band of 2007:

- Okumura-Hata path loss with the suburban correction. Base station height is
  the antenna's height above the phone's ground, 30 to 200 m.
- Knife-edge diffraction (ITU-R P.526) over the worst obstacle on the path,
  using mean ground height per cell and the 4/3-earth bulge. Obstacles within
  150 m of either end are ignored, because the Hata term already covers the
  buildings and trees around the phone.
- Each antenna is 50 m above its mast's placement. The phone is 1.5 m above
  the ground.
- Signal while total loss is 150 dB or less, a usual GSM maximum. Bars:
  120 dB or less is 4, 130 is 3, 140 is 2, 150 is 1.

Result for the eight sites:

| | 4 bars | 3 bars | 2 bars | 1 bar | No signal |
| --- | --- | --- | --- | --- | --- |
| Land | 15.9% | 22.8% | 34.3% | 15.5% | 11.5% |
| Water | 6.3% | 25.3% | 49.9% | 12.4% | 6.1% |

So 88.5% of the land and 93.9% of the water have some signal. Share of the
land each site reaches on its own:

| Site | Land |
| --- | --- |
| Upstate radio station | 58.9% |
| Missionary Hill (SF) | 45.5% |
| Cheyenne radio tower | 43.9% |
| radarmast1_LAwN (LS) | 37.0% |
| Carcer City masts | 26.8% |
| VC airport dish | 15.8% |
| masts1_sfe (SF) | 15.6% |
| Liberty radio | 3.6% |

The masts on high ground carry the network. Liberty's sits almost at sea
level and adds little. The dead zones are the large city in the river valley
north of the centre, pockets of the far west coast, the valleys around the
eastern city south of the lake, and the far side of the Upstate hills.

The same eight sites at other frequencies (land with any signal): 150 MHz
99.9%, 450 MHz 91.5%, 1800 MHz 49.5%. Those runs used the earlier method
described under Limits, which gives 91.9% at 900 MHz, so treat them as about
three points high.

## Limits

- The world tiles used for the heightmap have no ground for the large
  southern landmass. `coverage.py` treats land without tiles as flat ground at
  20 m, so there are no hill shadows there.
- Water uses the same suburban formula as land. Over open water, Hata's open
  area formula would give roughly one more bar far from the coast.
- The frequency comparison above was run before the switch from highest
  point per cell to mean ground height per cell for the mast ground level.

## Running it

Python 3 with numpy and Pillow.

```
python find_masts.py "<game folder>" masts-found.csv
python build_heightmap.py <folder of .glb world tiles> heightmap.npy
python coverage.py "<game folder>" heightmap.npy 900
```

`build_heightmap.py` needs 500 x 500 unit world tiles as glTF binary
(`<x>_<y>.glb`, world coordinates, Z up). They are made from the game's
models and are not in this repository. `coverage.py` reads
`Valkyrie-map-overview.txd` from the game folder for the land outline and
the picture under the result.

`coverage.py` writes `coverage-900.npy` (path loss in dB; row 0 is y = 16000,
column 0 is x = -5000, cells are 100 units) and two pictures, land only and
land with water. The pictures are drawn over the game's radar map, so keep
them on your own machine. They are not committed here.
