// Everything the phone needs, carried inside valkyrie-phone.asi itself.
//
// The build packs the phone's textures (valkyrie-phone.txd), its ringtones and
// text tones, and the web page pack when one has been built, into one blob and
// links it into the ASI as a resource. The game's own loaders want files, so
// on start the blob is unpacked once into a folder of the phone's own under
// Windows' temporary folder - never the game folder, which keeps only the ASI
// and its ini - and read from there. It is unpacked again only when the ASI
// carries something different.
#pragma once

#include <string>

namespace bundle {

// Unpack what the ASI carries. False when it carries nothing (a build without
// it): the phone then looks in the game folder, as it used to.
bool Unpack();

// The folder it was unpacked to, ending in '\', or empty.
const std::string& Dir();

// The unpacked file of that name (a path inside the bundle, such as
// "valkyrie-phone.txd" or "tones\\ringtones\\Chimes.wav"), or empty when the
// bundle has no such file.
std::string Path(const std::string& name);

}  // namespace bundle
