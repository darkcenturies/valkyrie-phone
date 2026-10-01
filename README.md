# Valkyrie Phone for GTA San Andreas

The SP-RP phone for GTA San Andreas 1.0 US: calls, messages, contacts,
camera, flashlight, maps, services and a trainer carried inside the ASI.
Press P to open it. The iFruit appearance is the default; `Skin=Keypad`
selects Valkyrie's optional keypad handset. Other original artwork is retained.

Build with `./valkyrie-asi-suite/build.ps1 -Release`. An x86 ASI loader is
required. Install with the game closed. Maps uses stock GTA SA tiles or tiles
built from the player's own game. Optional phone-as-a-weapon installation needs
modloader and a compatible fastman92 weapon type loader; follow both steps in
the generated package's README. The embedded trainer replaces a separate copy.

See [the detailed controls and configuration](valkyrie-asi-suite/valkyrie-phone/README.md)
and [source maintenance](MAINTENANCE.md). Native compilation and isolated tests
are separate from in-game startup, camera, device-reset and weapon validation.
The release packages now target GTA San Andreas only.

The repository contains no game files. Use your own copy of GTA San Andreas.
Original notices and third-party credits remain in THIRD_PARTY_NOTICES.md.
