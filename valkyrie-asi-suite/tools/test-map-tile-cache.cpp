#include "../valkyrie-radar/src/map_tile_cache.h"
#include <cassert>
#include <set>
#include <utility>
#include <cstdio>
int main() {
  const maptiles::Rect radar{3,5,-4,-2}, distant{3,5,8,10};
  assert(maptiles::Retains(radar,distant,true,4,-3));
  assert(maptiles::Retains(radar,distant,true,4,9));
  assert(!maptiles::Retains(radar,distant,true,4,3)); // corridor must be evicted
  assert(!maptiles::Retains(radar,distant,false,4,9)); // closed phone
  std::set<std::pair<int,int>> cache;
  for(int pan=0; pan<200; ++pan) {
    const maptiles::Rect phone{3,5,pan,pan+2};
    for(auto it=cache.begin();it!=cache.end();) {
      if(!maptiles::Retains(radar,phone,true,it->first,it->second))it=cache.erase(it);
      else ++it;
    }
    for(int y=radar.minY;y<=radar.maxY;++y)for(int x=3;x<=5;++x)cache.emplace(x,y);
    for(int y=phone.minY;y<=phone.maxY;++y)for(int x=3;x<=5;++x)cache.emplace(x,y);
    assert(cache.size()<=18); // distance travelled must not grow the cache
  }
  puts("Map tile retention: rapid pan, disjoint views and closed phone passed.");
}
