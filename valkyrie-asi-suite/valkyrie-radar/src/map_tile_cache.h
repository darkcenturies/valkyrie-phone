#pragma once
namespace maptiles {
struct Rect {
  int minX, maxX, minY, maxY;
  bool Contains(int x, int y) const {
    return x >= minX && x <= maxX && y >= minY && y <= maxY;
  }
};
inline bool Retains(const Rect& radar, const Rect& phone, bool phoneActive, int x, int y) {
  return radar.Contains(x,y) || (phoneActive && phone.Contains(x,y));
}
}
