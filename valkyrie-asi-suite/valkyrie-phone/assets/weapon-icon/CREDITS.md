# The phone's HUD weapon icon

`phone.png` is our own 16x16 pixel artwork, with a heavy black outline and
flat silver shading to match San Andreas's HUD. Regenerate it with
`tools/generate-weapon-icon.py`. The packer uses nearest-neighbour filtering
for `valkyriephoneicon`, keeping the enlarged pixels sharp.

`make-phone-weapon.ps1` packs it into `valkyriephone.txd` for the optional
phone weapon. Handset model textures are unchanged.
