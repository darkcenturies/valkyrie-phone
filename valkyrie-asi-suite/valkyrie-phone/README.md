# valkyrie Phone

`valkyrie-phone.asi` is the SP-RP server's phone rewritten for single player.
It is the iFruit, GTA V's phone, laid out the way the first iPhone it copies
is: the iFruit mark (the same mask valkyrie-radar carries) and an earpiece
above the 320 x 480 screen, the round home button below, the ring switch and
volume buttons down its left edge and the sleep button on its right; inside, a 20 point status bar with carrier and battery, 44
point navigation bars with arrow-shaped back buttons in the iFruit's light
blue, tab bars with a picture over each name, the home screen four across
with a dock of four, a keypad with Add Contact, Call and Delete, the in-call
screen's six buttons over End Call, grouped Settings, and the Clock's World
Clock, Alarm, Stopwatch and Timer. It is drawn in San Andreas' own style: a heavy black outline
like the HUD art, the subtitle font with its black edge, black help-box
panels, and icons drawn the way the game's radar icons are (16 pixels, hard
edges, a thick black outline, hard-banded shading and grain). Camera and Photos
have distinct bundled artwork. Toolbar, call-control, weather and game icons,
the camera buttons, and the clock dial share the same 16-pixel artwork.
Panels use stepped shading and controls have heavier borders. Settings retains GTA's original 16-pixel
radar_modGarage wrench. It and the cursor are the game's own,
loaded from the player's `models` folder.

In CJ's hand it can be a model of its own (`valkyrie-phone-model.dff` and
`.txd` next to the game; any SA replacement for `cellphone.dff` will do, such
as the iFruit one from LibertyCity.ru's "phone-michael" mod). The game's own
phone, model 330, is untouched: this phone's model stands in for it only while
this phone is out or at CJ's ear, so the story's calls still come in on the
game's phone. Without the files CJ holds the game's phone.

Like `valkyrie-fuel`, it is a port of the behaviour, not of the code: the
server's phone is Pawn with textdraws and a SQLite table, none of which exists
in a single-player process.

In multiplayer (S&SMP or SA-MP loaded) it stands down, because the server
already gives every player its own phone.

## Using it

| Input | Does |
| --- | --- |
| **P** | Take the phone out; CJ holds it in his left hand. It is an item of its own, not a weapon: his weapon is put away for it, no weapon can be drawn while he holds it, and the HUD's weapon icon shows the phone. Again: put it away. Set `Key` in `valkyrie-phone.ini`. |
| Mouse | A cursor appears on the phone; click to tap. Walking, the camera and firing are held while it is up. |
| Right mouse button | Lower the phone out of the way and get your controls back, for example during a call. **P** raises it again. |
| Esc | Put the phone away. |
| Swimming | The phone goes back in the pocket (a call stays at CJ's ear), and cannot be taken out until he is out of the water. |
| Keyboard | Types into whichever field is open. Enter sends, calls or saves. Up and Down move between a contact's name and number. |
| Home button (below the screen) | Back to the home screen, or out of a machine. |
| Back button (top left of an app) | Back one screen, as on the phone the iFruit copies. |
| Sleep button (right edge) | Press: the phone locks, to "slide to unlock". Hold: "slide to power off", with Cancel (or Esc) to back out. Held while the phone is off: switches it on. |
| Volume buttons (left edge) | The phone's own tones louder or quieter, ten steps; at the bottom they are off. |
| Ring switch (left edge, above the volume) | Silent: key clicks and text sounds are held back and the phone buzzes instead. The alarm still rings. |

## Keypad skin

Set `Skin=Keypad` in `[Phone]` for the optional valkyrie keypad handset.
The default remains iFruit. Both use GTA San Andreas contacts and services.


## Motion

Nothing on the phone just appears. It rises from the bottom of the screen
on a spring, running a hair past where it rests and settling back, with a
soft shadow under it; taken out, its screen lights up out of black; a screen deeper in an app slides in from the right and back out
the way it came, the one behind it dimming; an app grows out of its icon and
shrinks back into the home screen; the lock slides away and the icons fly in
from the edges with the dock rising under them; alerts rise as the screen
dims; lists coast when flicked, give past their ends and spring back; a new
text slides in from its own side; a call rises over the screen it was made
from, with "Calling..." counting its dots; notices drop in and lift away;
"slide to unlock" has the phone's band of light sweeping through it and its
knob springs back; a list being moved shows a thin scroll bar at its edge
that fades once it stops; the side buttons press
in, the volume and silent pictures fade in and out, the handset buzzes on
silent; the sleep button dims the screen out and back; and switching off
folds the picture to a line and a point like a tube, and brings it back up
out of black.

## The phone around the apps

The status bar is the phone's: the signal is full in the three cities and
fades out across the country between them, a bar less indoors and high up;
the EDGE "E" shows beside the carrier while the Internet or Maps is open;
the battery is the battery of the computer the game runs on, red when low,
with a bolt while it charges (full on a computer without one); and a
padlock stands where the time is while the phone is locked. Notices - a
number that can't be reached, a contact without a name, the hotline's
wait - drop down under the status bar over whatever screen is up.

If the phone is out but cannot be drawn for a couple of seconds of play (the
HUD hidden, another mod drawing over it), it is put away, so CJ is never
left holding a phone nobody can see with his weapons locked away.

## The look

The handset is built as the original iPhone is, and drawn exactly rather
than painted: the art generator works every pixel out from the shapes' own
distances, with its coverage for smooth edges, so there is no grain, no
outline and nothing to alias. A polished steel bezel rolls round the edge
(steep at the rim, flat where the glass sits in it); inside it one flat pane
of black glass covers the whole front, its edge ground a little round, the
display a hair beneath it; the earpiece is a slot through the glass with a
grille in it, the iFruit mark sits above it in silver, and the home button is
a hole in the glass with a shallow dish in it. The side buttons are steel
capsules standing out of the edge. No light is painted onto any of it. It is
lit per pixel by a small pixel shader (compiled at run time by Windows'
`d3dcompiler_47.dll`, the plainest profile the card takes) with the game's
own light: the sun where the time cycle puts it and as high as it stands,
and the ambient the game lights objects with - bright at noon, dim at night,
the colour of a room.

The shader is the textbook model rather than anything of the phone's own:
Lambert diffuse, a normalised Blinn-Phong highlight (a sharper surface gives
a smaller highlight, not a dimmer one) and Schlick's approximation of
Fresnel, so glass reflects 4% head on and far more at a glancing angle, and
metal reflects in its own colour with no diffuse. The view direction is
worked out per pixel from an eye in front of the game's screen, so a flat
pane catches the sun in a small glint that moves as the phone does; the
glint's peak is held down and all the added light rolls off softly toward
white, so it never blinds. Each part meets that light by its shape and what it is made
of, from two maps drawn with the handset: `phone_normal` (the edge rolling
off all the way round, over the outline and the band, the domed side
buttons, the flat glass a step below, the home button's dish) and
`phone_material` (gloss, smoothness, metal and how much it mirrors: glass,
polished steel, the dark polished edge, the dull rubber ring round the home
button, the silver mark). The three are drawn at twice the size the handset
is ever shown at and given every mip level, so its edges stay smooth at any
resolution.

Glossy parts reflect a copy of the completed world frame captured before the
2D/HUD pass, so the radar, HUD text and phone itself do not appear in the glass.
This does not add a second world render or change the game's mirror camera,
lighting or streaming. Only the Camera app uses the separate lens view.
`[Model] Reflections=0` uses the studio picture (`phone_env`) instead.

The screen sits under the glass as the original iPhone's does: the display,
a hair of air, then the glass, printed black round the opening. A second
pass, once the screen is drawn, lays the glass's reflection over it, and the
depth: seen at an angle the print's edge lies a little way over the
display's, so the picture's edge shifts as the phone moves, and the gap is
always a touch dark. Where the game's screen has pixels enough, the
display's own 320 x 480 pixel grid shows faintly. The screen gives its own
light. Without the shader compiler the handset is drawn plainly, tinted by
the same light.

The model sits in CJ's left hand. The game only ever draws a held model in
the right hand (a twin pistol's second copy aside), so the phone draws its
own copy of the cellphone model on the left hand's bone every frame, lit as
the game lights CJ, in the game's own weapon pass right after the world - so
CJ's body and fingers hide it where they should, as they would a gun. It is
turned half round about its own length (measured from the model) so its
screen faces CJ (`HandFlip=1`); `[Model] HandTurn` and `HandOffset` set how
it sits in the hand. At CJ's ear for a call the mission phone task has his right hand,
as in the story.

The model in CJ's hand reflects as the game's cars do: every material is
given the game's own car reflection (`vehicleenvmap128`) through
RenderWare's material effects, turning with the camera, as strong as
`[Model] Shine=` says. The home and side buttons sink a little when pressed
and click, a short tick of their own mixed over anything else playing (not
when the ring switch is on silent).

## What it does

Everything the server's phone does, apart from reaching other players,
because there are none:

- **Phone.** Keypad, Recents and Contacts tabs. Call any number. Contacts can
  be added (also straight from the keypad), edited, deleted, called and texted.
  There are 100 contacts at most, as on the server.
- **Service numbers.** These answer with the server's own lines, word for word:
  - **911** asks "Police, Fire or Medical", then "what is your emergency".
    When you answer, the game's own units are sent to you with sirens on:
    a patrol car for the city you are in, or a fire engine and an ambulance.
    They are built by `CCarCtrl::GenerateOneEmergencyServicesCar` and sent
    the way the mission-script wrapper sends them, then held as mission
    vehicles with a blue blip until they reach you (or for three minutes), so
    the game does not tidy them away on the road. The log follows each one.
    Nothing is sent to an interior, which is a room built far from any road.
  - **311** takes a message for the police, the fire department or city
    services.
  - **726**, the SAN hotline, takes a message by call or by text (one text a
    minute, as on the server).
  - **666** is the V-Rock hotel.
  - **100** saves the game. The phone asks first, then opens the game's own
    save menu, as a safehouse's save point does, so the save puts you back
    where you stood. Not in a vehicle and not during a mission.
  - **\*#87246#** (\*#TRAIN#) opens valkyrie-trainer's menu, as Alt+Z does,
    when the trainer is installed; the phone goes away first so the two do
    not fight over the mouse. A trainer that exports `valkyrieTrainerOpen` is
    asked directly; an older one is sent Alt+Z. Change it with `[Services] Trainer=`.
  - Any other number "can't be reached", as a number with no player behind it
    is on the server.

  911 units are made the way a mission script makes them: a road out of
  sight about 140 units away, the vehicle created there with a crew from
  that city's force, siren on, driving to you and following if you move. On
  arrival the crew gets out and car and crew are handed back to the game.
  The operator also takes one-tap answers (Shots fired, Robbery, ...) for
  when there is no time to type.

  What the other end says is shown as a subtitle and on the call screen.
- **Text.** Conversations, new messages, 128 characters a message, 128
  messages kept. Edit deletes a conversation. Texts to 726 are delivered;
  texts to any other number are marked Not Delivered.
- **Camera.** On the phone's own screen, upright, as the 2007 phone has it:
  the live picture above, and below it the last photo, the shutter and the
  switch to the front camera. CJ holds the phone up (or out at arm's length
  for a selfie); the game's own view is left as it is, so you see him
  taking it. The picture on the phone is the
  world drawn a second time from the phone's lens, through the game's own
  mirror renderer (`src/viewfinder.cpp`; see "The viewfinder" below). The
  mouse aims - it turns CJ and tilts the phone - the wheel zooms, a click
  takes the picture, F turns the phone round, G opens
  the Camera Roll, Esc, Backspace or the phone's key goes back, and the right button lowers the phone. Pictures
  are the game's own photos: for the frames the picture is taken the game's
  camera is put at the lens, under the flash, and put back straight after, and the game saves what it
  sees to the Gallery folder of the game's User Files, exactly as the
  in-game camera does (the menu's "save photos" is switched on for each
  one). With `[Photos] Shape=Portrait` the saved picture is then cut to the
  viewfinder's upright shape.
- **CJ's phone actions.** Eighteen original upper-body clips are embedded in
  the ASI. The left hand holds the phone; actions include taking it out,
  lowered holding, reading, typing, calls, rear-camera photos, selfies,
  shutter presses and putting it away. Camera flips and exits have their own
  transitions. No separate animation installation is needed.
  Story calls, falls, swimming, vehicles and protected tasks take priority.
  Cleanup fades only the phone's association. `[Animations]` selects clip,
  block and looping; old shipped placeholders upgrade automatically while
  custom choices remain. See [animation source](assets/animations/README.md).
  This test build still needs in-game checks of motion and handset alignment.
- **Photos.** The Camera Roll (the Gallery folder, newest first, with the
  camera one tap away and pictures thrown into the Windows Recycle Bin) and
  the server's 27 wallpapers, with their SP-RP names.
- **Clock.** The game's time and weekday, and an alarm on the game's clock
  that rings with your ringtone.
- **Calculator** and **Notes.**
- **Sounds.** The SP-RP phone's own: the game's ring (mission audio 20600)
  as the ringtone, its text alert (40405), and its ringback (3600) while a
  call rings out. Beside those, 13 ringtones and 9 text tones made by
  `tools/generate-phone-tones.py` from sine waves, plucked strings and noise.
  Settings lists them all; a tap plays one and picks it. Any `.wav` put in
  `valkyrie-phone-tones\ringtones` or `\texttones` in the game folder is
  listed too.
- **CJ's contacts.** The people from the story are in Contacts beside your
  own. None of them picks up.
- **Settings.** Wallpaper; the ringtone and text tone, with Lock Sounds
  (the phone's click as it locks and unlocks) and Keyboard Clicks (a tick
  for every key typed), both held back by the ring switch; Silent and
  Volume; Auto-Brightness (the display turns itself down a little in the
  dark, as the phone's light sensor has it do); Auto-Lock (1, 2 or 5
  minutes, or Never - left alone the screen dims ten seconds before, then
  locks to "slide to unlock"; never in a call, the camera, a game or with an
  alarm up); slide to unlock on or off; and About. About is
  the phone's own "valkyrie OS" page, laid out as the original iPhone's
  Settings > General > About: the system and its version over a grouped list
  of what is on the phone (your number, which uses the server's rule: 160000
  plus up to 9999, and how many contacts, messages and calls it holds), then
  the same words, thanks and links the trainer's About page carries - Ko-fi
  and the projects elsewhere, each opened in the browser when tapped. The
  logos come from the trainer's own art (`brands()` in the art generator).
- **Games.** San Andreas' four arcade machines, ported from their mission
  scripts in `main.scm` with the scripts' rules, numbers, sprites (from the
  player's own `models\txd\LD_*.txd`) and high-score tables: Duality, Let's
  Get Ready To Bumble, They Crawled From Uranus and Go Go Space Monkey. Each
  one is laid out again for the phone's upright screen, on a 448 x 672 canvas
  so every sprite keeps the size its script gives it. Go Go Space Monkey is
  turned a quarter round and flies up the screen. Arrows or WASD steer, Space
  or Enter is the machine's main button, the left mouse button or Ctrl
  shoots, Q and E strafe in Duality, Shift brakes in Bumble, Backspace backs
  out; the home button or Esc leaves the machine. High scores are saved with
  the phone's data.
- **Maps.** valkyrie-radar's own 3D world - the website's baked map tiles
  the radar draws - seen from above anywhere on the map. Drag to move it,
  the wheel or - and + to zoom from a street out to a few districts, 3D to
  lean the view over the rooftops, Me to go back to following CJ, whose
  arrow is the game's own radar arrow. The radar renders it on its isolated
  device (`valkyrieRadarPhoneMap`, beside its HUD panel's own capture,
  touching none of that panel's state) and says where CJ falls on it
  (`valkyrieRadarPhoneMapProject`). Each tile is a full-detail mesh with a
  large texture, so the view reaches only so far from its middle, new tiles
  are read one a frame, the nearest first, and the picture is drawn again
  only when the view moves or a tile arrives. It needs valkyrie-radar with its tile
  set; without it the app says so. An older `valkyrie-phone.ini` is given
  the app once, after Photos (`[Apps] Added=` remembers).
- **Internet.** An offline in-game browser with bundled pages. Public Actions
  builds include eight authored San Andreas pages across six sites. GTA IV
  pages are optional local content converted from the player's own GTA IV
  install; see the build instructions below. Drag or wheel to scroll,
  double-tap to zoom, and tap links to follow them. Back, forward, Home and
  the site list navigate the pages included in your build.

Contacts, messages, recent calls and settings are saved to
`valkyrie-phone.dat` in the GTA San Andreas User Files folder, beside the save
games.

During a call CJ puts the phone to his ear: the plugin runs the game's own
`TASK_USE_MOBILE_PHONE` opcode (0729), the one missions use, through the
script interpreter (`valkyrie-core/src/script.h`).

Not done yet: the taxi number, and the machines' sounds.

## The internet

The public Actions download carries **eight authored pages across six sites**:
Cluckin' Bell (home/menu), Epsilon (home/join), eXsorbeo, Maccer,
West Coast Rap Legends and sp-rp.com. They come from `tools/sa-web`.
It includes **no GTA IV pages and no archived Rockstar promotional pages**.
The app works offline; it is not a general web browser. Links marked external
open your normal desktop browser when tapped. Home opens sp-rp.com when
Eyefind is absent.

A clean public build creates `build/valkyrie-web.dat` using `--sa-only
--no-archive`, and embeds it in the ASI. It requires Python, Pillow,
Playwright and Chrome or Playwright's Chromium. Nothing is fetched while
playing. A previously generated `build/valkyrie-web.dat` is reused; an optional
local kept pack under `assets/web` is also supported, but neither pack is
tracked in this public repository.

From the repository root, prepare the page builder and build:

```powershell
python -m pip install Pillow playwright
python -m playwright install chromium
./valkyrie-asi-suite/build.ps1 -Release
```

For **GTA IV websites**, supply your own GTA IV install containing `pc/html`
and `pc/text/american.gxt`:

```powershell
./valkyrie-asi-suite/build.ps1 -Release -Gta4Path "C:\Games\Grand Theft Auto IV\GTAIV"
```

An explicit `-Gta4Path` rebuilds the page pack, including the authored SA
pages, even when a previous pack exists. Check the build's page conversion
messages: explicitly requested GTA IV input must exist and convert successfully,
or the build fails. This local
build differs from the public Actions download. GTA IV files and derived
pages are not committed or distributed by Actions.

To restore a page pack from a previous personal build instead of converting
GTA IV again, use its existing VWEB file:

```powershell
./valkyrie-asi-suite/build.ps1 -Release -WebPackPath "C:\PhoneAssets\valkyrie-web.dat"
```

This embeds that supplied local content in your ASI and prints its page count.
It is a personal build input, not content included in the Actions artifact.
Use one of `-Gta4Path` or `-WebPackPath`. Neither uploads the input to GitHub.

`tools/iv-web/whm.py` reads GTA IV's `.whm` pages. The converter reconstructs
HTML, reflows it for a 320-point screen, and stores page images plus link
rectangles. `american.gxt` supplies text used by pages such as Eyefind,
Craplist and the LCPD database. Content generated by GTA IV scripts at runtime,
such as news stories or inbox messages, may remain blank. `--desktop` retains
the original desktop layout. The direct pack-builder CLI also offers optional
archive imports; public Actions always uses `--no-archive`.

## The viewfinder

San Andreas can already draw the world a second time from somewhere other
than the game camera: its mirrors work that way, and so do the stadium's big
screens, which show the race from cameras round the track. Each frame
`CMirrors::BeforeConstructRenderList` asks the game camera where the mirror
camera is (`CCamera::DealWithMirrorBeforeConstructRenderList`), the render
list takes in what that camera can see, and `CMirrors::BeforeMainRender`
renders the scene from it into a texture before the main picture.

While the Camera app is open the phone takes that over. The mirror camera
is the phone's lens and the texture is the game's own mirror buffer, made by
`CMirrors::CreateBuffer` as it is for a mirror; `CMirrors::ShutDown` is held
off so the buffer is not freed every frame outside a mirror room, and
`CMirrors::RenderMirrorBuffer`, which would lay the texture over a mirror in
the world, is skipped. The phone's screen draws the buffer instead. When the
app closes every byte goes back. A real mirror in the room shows nothing
while the camera is up.

## valkyrie-phone.ini

Everything that can be changed is in one file, written next to the game the
first time the phone starts:

- `[Phone]`: the key, the phone's height, which side it sits on, and the model CJ holds.
  `IconSize=16` keeps the default pixel icons; `IconSize=64` selects the older detailed set.
  The original Settings wrench stays unchanged in both modes.
- `[Apps]` `Order=`: which apps are on the home screen and in what order.
- `[Icons]`: each app's icon, from the phone's own art, the game's radar
  icons (`hud:radar_...`), or a `.png` in the game folder.
- `[Labels]`: the name under each icon.
- `[Contacts]`: CJ's contacts, `Name=Number`.
- `[Ringtones]`, `[TextTones]`, `[Sounds]`: the tones to pick from, the
  ringback, and key and sent sounds - the game's sounds by number or `.wav`
  files.
- `[Services]`: the numbers that answer, and `Save=` for the save number.
- `[Camera]`: the mouse's `Sensitivity=` when aiming.
- `[Photos]`: `Shape=Portrait` or `Screen`.
- `[Animations]`: the animation, `.ifp` and looping of each pose (`Use`,
  `Camera`, `Selfie`), and where the lens sits in the two camera poses
  (`CameraLens=`, `SelfieLens=`, metres to CJ's right, in front, up). An
  animation that will not play is written to the log and left out.
- `[Internet]` `Home=`.

A section taken out falls back to what ships; a section a newer phone adds
is appended to an older file without touching the rest.

## Art

`assets/sprp` and `assets/wallpapers` are decoded from the server's
`models/phone.txd` and `models/phone_nice.txd`. `tools/generate-phone-art.py`
draws the handset and the icons from them into `assets/generated`,
and writes `src/phone_art.h` with the positions on the body texture.
`build.ps1` packs `assets/generated` into `valkyrie-phone.txd` with
`tools/pack-phone-txd.ps1`, so a build needs no Python. Run the generator again
after changing a drawing.

## Files

| File | Where |
| --- | --- |
| `valkyrie-phone.asi` | game folder - everything the phone needs is inside it (below) |
| `build/valkyrie-web.dat` | generated offline page pack embedded in the ASI; not tracked |
| `valkyrie-phone-model.dff`, `valkyrie-phone-model.txd` | game folder, optional: the model CJ holds (see above); not part of this project |
| `valkyrie-phone.ini` | game folder, written with every setting and a note on each the first time the phone starts (see below) |
| `valkyrie-phone.log` | game folder, development builds only |
| `valkyrie-phone.dat` | GTA San Andreas User Files |

The phone ships as its ASI and its ini, nothing else. The build packs its
textures (`valkyrie-phone.txd`, from `assets/generated`), its ringtones and
text tones (`assets/tones`) and the web page pack (`build\valkyrie-web.dat`,
when one has been built) into one blob linked into the ASI as a resource
(`src/bundle.cpp`). On start the phone unpacks it once into a folder of its
own under Windows' temporary folder - never the game folder - and reads it
from there; a new build unpacks afresh. Installing takes away the loose
copies older builds left beside the game. A player's own `.wav` files in
`valkyrie-phone-tones\ringtones` or `texttones` in the game folder are still
listed with the phone's own. At run time the phone opens no network
connection; only its links out (Ko-fi, sp-rp.com) hand an address to the
player's browser, and only when tapped.

## Shared code

The phone added three modules to `valkyrie-core`, which any plugin can use:

- `sprite.h`: load a loose `.txd` into a slot of its own, draw its textures
  with `CSprite2d`, make textures from pixels at run time, and draw text that
  is centred, right-aligned, wrapped or cut to fit.
- `script.h`: run one of the game's own script opcodes through
  `CRunningScript::ProcessOneCommand`, as a line of main.scm would.
- `input.h`: capture the mouse and keyboard from the game. It gives you a
  cursor driven by the game's own `CPad::UpdateMouse`, and typed keys through
  the window procedure, while the game sees a still mouse and no key presses.

## GTA San Andreas content

The phone uses Carl Johnson's contacts, services and stock signal grid.
`Skin=Keypad` selects the optional keypad appearance; iFruit is the default.
