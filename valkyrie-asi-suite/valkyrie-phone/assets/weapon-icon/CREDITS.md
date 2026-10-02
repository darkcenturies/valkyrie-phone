# The phone's HUD weapon icon

`phone-source.png` is our own original inventory phone artwork, made by
valkyrie-inventory's make-phone-icon.py. `tools/generate-weapon-icon.py`
prepares a 64x64 PNG and DXT3-compressed DDS, matching the dimensions and
compression of the stock San Andreas fist in `hud.txd`.

`make-phone-weapon.ps1` packs the DDS into `valkyriephone.txd` for the optional
phone weapon. The HUD uses point filtering. Handset model textures are unchanged.
