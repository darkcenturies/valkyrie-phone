// GTA IV's internet, from valkyrie-web.dat.
//
// The pack is built on the player's machine from their own copy of GTA IV
// (tools\iv-web\build-web-pack.py): every page laid out and saved as a
// picture, with the rectangles of its links. This reads it, decodes the page
// being looked at on a thread of its own, and hands it to the phone as
// textures to draw.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace web {

constexpr int kNoPage = -1;
constexpr int kBack = -2;  // a link the page means as the browser's back
constexpr int kExternal = -3;  // a link out to the real internet, opened in the player's browser

// Read the pack's index. False when it is missing or not a pack.
bool Open(const std::string& path);
bool Available();

int PageCount();
struct Page {
    std::string site, name, title;
    int width, height;  // pixels
};
const Page& Info(int page);
int Find(const std::string& site, const std::string& name);

struct Link {
    int target;  // a page, kNoPage, kBack or kExternal
    float x, y, w, h;
    std::string url;  // kExternal's address
};
const std::vector<Link>& Links(int page);

// Every site, by name, with the page to open it at.
struct Site {
    std::string name, title;
    int home;
};
const std::vector<Site>& Sites();

// Start loading a page; call every frame while it is wanted. True once its
// textures are ready to draw. Only one page is held at a time.
bool Load(int page);

// Draw the loaded page with its top left at (left, top) in screen pixels,
// `scale` pixels to a page pixel. Draw inside a scissor: the page's tiles are
// not cut to the view.
void Draw(float left, float top, float scale, float viewTop, float viewBottom);

// Let go of the loaded page's textures.
void Release();

}  // namespace web
