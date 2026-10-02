# Third-party notices

The phone includes, or builds with, the following. Each keeps its own
licence; the texts are in `valkyrie-trainer/bundle/licenses/` and are also
carried inside the built ASI.

| Component | Where | Licence |
| --- | --- | --- |
| plugin-sdk (DK22Pac and contributors) | `valkyrie-trainer/third_party/plugin-sdk` (submodule) | MIT |
| Dear ImGui (Omar Cornut and contributors) | `valkyrie-trainer/third_party/imgui` (submodule) | MIT |
| SafetyHook | inside plugin-sdk (`safetyhook/`) | Boost Software License 1.0 |
| Zydis and Zycore | inside plugin-sdk (`safetyhook/Zydis.c`) | MIT |
| Hooking (Bas Timmer / NTAuthority et al.) | inside plugin-sdk | MIT |

## Game content

No Grand Theft Auto files are in this repository. At run time the phone reads
from the player's own game: its radar icons and cursor (`models/hud.txd`,
`models/fronten_pc.txd`), the arcade machines' graphics (`models/txd/LD_*.txd`),
its fonts, sounds and models. The browser's pages are built on the player's
machine from their own copy of GTA IV.

The map tiles for the Maps app are rendered from the games' own models and
textures. The full Actions package includes 142 generated stock SA tile pairs;
these binary release assets are kept outside the source repository. Rockstar
Games retains ownership of the underlying GTA San Andreas game content.

## Optional weapon configuration

The optional package requires Modloader 0.3.10 (thelink2012 and contributors) and
fastman92 limit adjuster 7.6 (fastman92), installed separately. Their binaries
are not bundled. The replacement weapon configuration retains fastman92's stock
7.6 template and author credit, with the phone's weapon entry added. The supplied
INI enables the weapon loader and preserves its required author field. These
replacements target stock SA plus the phone; existing custom settings need to be
retained when combining mods.

## Artwork

The phone's icons, handset, model, wallpapers and tones are made for this
project. `valkyrie-phone/assets/sprp` and `valkyrie-phone/assets/wallpapers`
are SP-RP's own phone artwork. The iFruit name and mark come from the Grand
Theft Auto series and are used as the in-world brand of the phone.
