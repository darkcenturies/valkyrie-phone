# Phone art

- `sprp/` and `wallpapers/`: textures decoded from the SP-RP server's
  `models/phone.txd` and `models/phone_nice.txd` (the wallpapers, the battery,
  the contact glyph). SP-RP's own artwork, reused as it is.
- `generated/`: what `../tools/generate-phone-art.py` draws - the handset and
  the icons in San Andreas' HUD style, and the wallpapers cut to the 2:3
  screen. This is the folder `build.ps1` packs into `valkyrie-phone.txd`. Do
  not edit these by hand; change the script and run it again.

The game's own radar icons and mouse cursor are not copied here: the plugin
loads them from the player's `models/hud.txd` and `models/fronten_pc.txd`.
