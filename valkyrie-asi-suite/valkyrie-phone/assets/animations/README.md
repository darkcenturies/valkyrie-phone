# Phone animations

`valkyrie-phone.ifp` contains 18 original clips authored by
`../../tools/generate-phone-animations.py`. No stock animation keyframes are
copied. The pack is embedded in the ASI and does not require Modloader.

The left hand holds the handset. Clips cover taking it out and putting it away,
lowered holding, reading, typing, calls, rear-camera use, selfies, shutter presses
and transitions between those actions. Only upper-body bones are animated;
locomotion remains under the game's control. Story calls, swimming, vehicles,
falling and protected tasks interrupt the phone actions.

To regenerate, install NumPy and run the generator. `--check` verifies the checked-in
pack without rewriting it. `tools/test-phone-animation-pack.py` checks the ANP3
layout, bone scope, quaternion continuity and loop endpoints. Visual motion and
handset alignment still require testing in GTA SA 1.0 US.

The optional `*_64.png` artwork in `../generated` restores this project's older
artwork from public commit `a0cc351`; the default artwork remains 16×16. The
Settings icon continues to use the game's original wrench in both modes.
