// Turning parts of the plugin off without rebuilding it.
//
// This exists because two crashes arrived in quick succession with three
// changes in flight - per-model animations, the Manhunt swap and the map zoom -
// and no way to tell which was responsible short of a rebuild per guess. A
// marker file beside gta_sa.exe is something the owner can create in seconds.
//
//   sprp-noanims.off       no animation work at all
//   sprp-nomanhunt.off     per-model animations stay, the Manhunt swap stops
//   sprp-nozoom.off        the map zoom is left exactly as the game does it
//   sprp-noradarshape.off  the radar's corner mask is left alone (stays round)
//
// Checked once, at startup, and written to the log so a crash report says which
// were in force.
#pragma once

namespace switches {

void Read();
void ReadMap();
void ReadAnimations();

bool AnimsOff();
bool ManhuntOff();
bool ZoomOff();
bool RadarShapeOff();

}  // namespace switches
