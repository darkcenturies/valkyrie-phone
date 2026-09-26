# Valkyrie Trainer — single-ASI tester build

## Local travel update — 2026.09.23-r3

Teleport now leads with **Teleport to waypoint** (also available in Controls).
It resolves the actual placed marker from the relocated radar array through
Valkyrie Core, rather than using the pause-map cursor. Unsupported layouts,
missing markers and stale handles fail without moving the player.

Destinations come from zone files referenced by the installed `data/gta.dat`.
The tested Project Eagle installation supplies 371 unique destination keys,
including 15 city/region shortcuts and the additional districts and landmarks.
Search accepts city/district names; **Surprise me** chooses a destination.
Malformed zone records are excluded. No destination files are shipped or extracted.

Travel is queued onto the game update. It streams collision, finds a dry surface
with a walkable normal, and places the ped or upright vehicle above that surface.
Zone destinations try up to nine points within their bounds. Missing collision,
water or steep terrain cancels travel; waypoint travel never silently substitutes
another XY location. Go Back and saved locations preserve their original height.
Outdoor destinations require leaving interiors. Exact-height custom travel is
available by disabling **Place on ground**.

Build, embedded-resource checks, relocated-waypoint/parser tests against the
installed zone files, animation controller/native-backend tests and action-key
tests pass. Native collision, all individual destinations and map-marker travel
still require in-game testing. The published r2 download is separate from this
local r3 build.

The current release ships only `valkyrie-trainer.asi`. Font, weapon textures,
project logos, credits and license notices are compiled as resources and loaded
directly from memory. No asset files are extracted. Existing action/preset/save
configuration and diagnostic logs remain separate runtime files.

Close GTA and replace the old trainer ASI beside the game executable. Alt+Z opens
the trainer; F9 stops animations. Preview rotation starts enabled; dragging pauses
rotation and Reset view restores the initial angle, zoom and rotation.

Build with `build.bat` and the reviewed `bundle/` inputs. The resource compiler
runs before linking. Embedded content is shown under Info, including licenses.
The x86 build, resource byte checks, animation tests and native UI previews pass;
in-game confirmation remains pending. Earlier installation notes below describe
the previous multi-file tester package and do not apply to r2.

Installed locally with a backup of the previous ASI. See install-status.txt for its hash and backup path. The trainer continues to link Valkyrie Core for layout checks, relocated model access, logs and instrumentation.

## Animation repair following in-game failure

Read trainer-animation-repair.md first. The prior audit missed real playback failures. This build corrects the CJ preview's static-function calling convention and uses the scripted primary task slot while preserving the default player task. Blend-out is no longer mistaken for completion.

Animations now exposes Play once / Loop full clip / Hold last frame, Pause/Resume, Restart, timeline seeking, First/Last frame and playback speed. Seeking pauses at the chosen pose. Stop/F9 releases it; movement with the menu closed cancels playback. New controls are available for action bindings. Updated isolated tests pass; in-game confirmation is still pending.

## Menu, controls and Info

This pass reviews all ten trainer pages. Read trainer-design-review.md for the page-by-page findings, implemented changes, research references and validation limits.

- The header is the Valkyrie Trainer wordmark only. No portrait or logo footer appears on working pages.
- Readable body text, visible slanted action buttons, labelled input fields, checkboxes, named weapon rows and small original game icons replace ambiguous decorative controls.
- Weapons use selection, ammunition and Give Weapon. Vehicles and Skin place the apply button beside the preview. Teleport selects a destination before moving. Animation details, sequences, traffic effects and less-used tools are grouped. Saved tools expose availability; Controls uses aligned binding columns.
- Info has About, How to use, and Credits & links. The introduction uses lowercase **valkyrie** and plain, casual copy. The guide explains all pages, shortcuts and limits.
- Logos appear only in Info: Valkyrie, SP-RP, Project Eagle, S&SMP, Project Silent Hill. They retain transparent backgrounds and hover colour. The correct supplied Silent Hill image is included.
- Valkyrie opens https://ko-fi.com/valkyriesamp; SP-RP opens https://sp-rp.com/; Project Eagle opens https://www.projecteaglemod.games/; Project Silent Hill opens https://discord.gg/WRthyZNdWS. S&SMP remains logo only. Links open only after a click.

## Final audit update

Read trainer-final-audit.md for findings, test coverage and remaining in-game checks. Every sidebar section now has a game icon. Dynamic status/loading text keeps a fixed layout. Small muted links sit below the credits under “elsewhere,” with hover shading but no URL popup.

Skin includes Return to CJ and CJ clothes (choose category/item, then Wear selected; accessories can be removed; Restore outfit undoes the session's clothing edits). Player includes Restore Movement. Animations now release held poses on Stop, allow interruption, distinguish completion from interrupted tasks, and validate sequence/preset data. CJ's preview shoulder correction is no longer overwritten while reading its orientation.

A caught trainer fault disables the trainer for the session rather than retrying the same operation every frame. Restart the game after that message. This does not guarantee recovery from every native failure.

## Rendering fixes

Model 49412 has nine correctly loaded materials, but its legacy triangle table assigns all 1,610 triangles to material 0. Read-only inspection confirmed that its rendering BinMesh contains nine correct material groups and 4,830 indices. The preview now uses these rendering meshes, supporting triangle lists and strips, with the old triangle data as a fallback. This fixes the shirt texture being applied across the head, arms, trousers and other surfaces on affected exports.

Repeated selection now explicitly releases and reloads the preview instead of only changing its loading message. Vehicle paint still cycles through the installed carcols variations. The transparent renderer applies the Inventory/Atmosphere exterior-only animated ink kernel on the GPU; it does not paint over the model's surface.

CJ uses the isolated dressed-rig setup from the existing Inventory/Atmosphere portrait: cloned hierarchy, clothing rebinding, preserved rest offsets, default idle and CJ's post-animation correction. It never animates the live player to generate the preview.

## Controls and tools

Alt+Z opens the trainer. Use Controls to find and bind actions, or right-click a supported button/toggle. Bindings support Ctrl, Shift and Alt, reject duplicates/reserved combinations, and persist in valkyrie-actions.cfg. They work across tabs and with the trainer closed. Typing, focus loss and game pause suppress execution. Existing F5–F8 animation presets and F9 stop remain reserved.

Autowalk is an on-foot toggle on Player and can be bound. It uses a modest forward input when manual forward/back input is absent. It pauses while the trainer is open and turns off on focus loss, game pause, vehicle entry, death or Escape. It does not install or replace a movement task.

Extras adds persistent Save/Recall Location, Save/Recall Loadout and Save/Recall Ride, plus Random Ride, Sunshine, Stormy Night, Beach Day and Go Back. Vehicle repair invokes the vehicle's actual repair routine; upright resets rotation and motion. Teleport carries the occupied vehicle and streams the destination. Saved locations and Go Back currently require the same interior/area. Saved rides store the model, not full tuning or paint. Selection-based hotkeys use the current menu selection. Quick saves persist in valkyrie-quick-state.cfg.

## Validation and installation

The x86 build passes. Tests cover rendering/material groups, list/strip winding, actual D3D9 textured pixels, transparent background, exterior-only animated ink, state restoration/device reset; keybind persistence/conflicts/reserved keys and autowalk input policy; and the existing animation loading/sequence lifecycle. Native UI previews were inspected at 1280x900, 800x600 and the 800x520 navigation layout with sample state and original weapon artwork. Info tab navigation was exercised with synthetic mouse clicks. All ten page implementations were reviewed in code. These previews are not in-game screenshots.

In-game visual and interaction verification remains pending, especially CJ and model 49412. This is a broader action framework and initial useful/fun toolset, not a claim to cover every possible trainer feature.

Install valkyrie-trainer.asi, pricedown.ttf, valkyrie-trainer-weapons.txd and the valkyrie-trainer-assets folder alongside the game executable while GTA is closed. The icon dictionary is copied unchanged from the existing local Inventory artwork. Info assets use the repair tool Valkyrie/SP-RP artwork, original transparent Project Eagle and S&SMP artwork, and the exact supplied project-silent-hill-icon-1024.png.
