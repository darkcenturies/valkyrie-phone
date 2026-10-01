# GTA San Andreas mod update workflow

The current mods target classic GTA San Andreas PC 1.0, with individually
verified native signatures. Their product UI and new packages have no former
partner editions or branded profiles. Historical research archives and
mandatory third-party attribution are preserved as evidence.

CLEO development uses [Dryxio CLEO AI](https://github.com/Dryxio/cleo-ai/tree/35e60c3f7037e39dcb73d415dca16e657125f398)
at revision `35e60c3f7037e39dcb73d415dca16e657125f398`. Read its README and
AGENTS.md. Run `python tools/sync_reference.py` in that external tool checkout
to reproduce its pinned opcode catalog. The profile is GTA SA PC 1.0,
CLEO 5.4.0, Sanny Builder 4.2.0 (`sa_sbl`). Look up commands/signatures before
authoring, validate, compile with Sanny and record a separate runtime result.
Extensions require explicit declarations. No third-party CLEO AI code is
copied into this repository.

Run `python tools/check-cleo-workflow.py --cleo-ai PATH` from this repository.
It checks the upstream revision/profile and sends every actual CLEO source
under valkyrie-asi-suite/ through that validator. C# Repair sources are not CLEO scripts.
The current ASI mods are native C++: the script validator cannot validate
their memory hooks, calling conventions or C++ behavior. Zero CLEO sources
is reported as not applicable, never as a native compatibility pass.

For native changes, build with `./valkyrie-asi-suite/build.ps1 -Release`
and run the existing startup tests. The public Phone build/package scripts,
notices and router include paths remain local adaptations. Shared changes
are reconciled through the integration repository's reviewed export manifest.
Native and CLEO checks complement one another. Only publish merged clean main;
in-game startup, camera, weapon and device-reset testing remains separate.
