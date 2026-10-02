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

The one-file map builder is generated from readable `tools/map-data` sources;
the reviewed import manifest pins the standalone Radar inputs. Keep game-derived
output out of Git. Validate changes with `tools/make-phone-map-builder.py --check`
and `tools/test-phone-ui.ps1`, as well as the native build and startup tests.

Other homes:

- `darkcenturies/valkyrie-workshop`: shared development, Atmosphere and private mods.
- `darkcenturies/gta-workshop`: public guides, findings, generated research and
  source for the ten defined Valkyrie tool families under `tooling/`. Mod
  implementation and mod releases remain outside that reference library.
Mod implementations stay outside the public tooling repository.
The content/signal scripts here remain compatible consumer copies of the
published tool families. Review path/hash changes deliberately; do not replace
Phone's build-specific scripts or assets with a bulk tooling export.
The old signal-coverage study branch is retained for research and is not the
release source. Start maintenance from current main; do not merge older study
branches wholesale over newer features.

The phone retains camera-control cleanup, a forward handset light and
finished-scene capture. Standalone startup
now chains RenderWare initialization on the game thread; map initialization
follows configuration without worker sleeps. GTA remains the default profile.
No game assets are imported here.

Build with `valkyrie-asi-suite/build.ps1`. The native startup regression test is
`valkyrie-asi-suite/tools/test-game-startup.cpp` (MSVC x86, C++17, include
`valkyrie-asi-suite/valkyrie-core/src`). It checks chaining, deferred one-shot
execution, duplicate registration, invalid opcodes and original return values.
In-game startup, camera exits, map/device resets and flashlight checks remain
required on supported game builds before a binary release.
