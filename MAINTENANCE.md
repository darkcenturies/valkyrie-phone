# Source ownership and maintenance

This is the canonical public phone repository and builds independently.
Shared phone/core/map development also lives in the Valkyrie workshop integration
repository. Its reviewed `tools/phone-sync.json` records the shared files and
hashes, and `tools/sync-phone.py` detects drift before applying an export.
Do not copy an entire private repository or Git history into this one.

The public build/package scripts, notices and router include paths are local
adaptations. Preserve them when bringing over shared fixes. Contributions here
must be reconciled into the integration source before its next export; the
sync tool refuses to overwrite independent changes.

Other homes:

- `darkcenturies/valkyrie-workshop`: shared development, Atmosphere and private mods.
- `darkcenturies/sp-rp-public-research`: public Doctor, Crashfix, Repair, earlier
  Map and research; its release scope does not expand with a phone change.
- `FrankoU28/Project_Silent_Hill`: the PSH consumer integration and game-specific
  assets. Its no-trainer build is not the general phone build.

The old signal-coverage study branch is retained for research and is not the
release source. Start maintenance from current main; do not merge older study
branches wholesale over newer features.

The Silent Hill work contributes an optional profile/skin, camera-control
cleanup, a forward handset light and finished-scene capture. Standalone startup
now chains RenderWare initialization on the game thread; map initialization
follows configuration without worker sleeps. GTA remains the default profile.
No PSH map tiles or other game assets are imported here.

Build with `valkyrie-asi-suite/build.ps1`. The native startup regression test is
`valkyrie-asi-suite/tools/test-game-startup.cpp` (MSVC x86, C++17, include
`valkyrie-asi-suite/valkyrie-core/src`). It checks chaining, deferred one-shot
execution, duplicate registration, invalid opcodes and original return values.
In-game startup, camera exits, map/device resets and flashlight checks remain
required on supported game builds before a binary release.
