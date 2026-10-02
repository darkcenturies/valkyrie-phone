#include "water_mesh.h"
#include <cassert>
#include <sstream>
#include <cstdio>

void TestRadarWater() {
    std::istringstream data(
        "processed\n"
        "0 0 27 0 0 1 0  100 0 27 0 0 1 0  0 100 27 0 0 1 0  100 100 27 0 0 1 0 1\n"
        "200 200 5 0 0 1 0  300 200 5 0 0 1 0  200 300 5 0 0 1 0 1\n"
        "broken 1 2\n"
        "0 0 0 0 0 0 0 1\n");
    auto patches=radar_water::Read(data);
    assert(patches.size()==3);
    assert(radar_water::Mesh(patches,-50,-50,20).empty()); // Dry land stays dry.
    assert(radar_water::Mesh(patches,290,290,5).empty()); // Outside triangular lake.
    auto lake=radar_water::Mesh(patches,50,50,20);
    assert(!lake.empty() && lake.size()%3==0);
    float area=0;
    for (size_t i=0;i<lake.size();i+=3) {
        auto a=lake[i],b=lake[i+1],c=lake[i+2];
        area+=std::abs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x))*.5f;
        for (unsigned j=0;j<3;++j) {
            const auto p=lake[i+j];
            assert(p.x>=30 && p.x<=70 && p.y>=30 && p.y<=70 && p.z==27);
            assert(radar_water::DistanceSquared(p,lake[i+(j+1)%3])<=32.f*32.f+.01f);
        }
    }
    assert(std::abs(area-1600.f)<.01f); // Quad clipping neither loses nor duplicates area.
    std::puts("PASS: dry terrain, triangular water footprints, elevated lakes, clipping and fog tessellation");
}
#ifdef RADAR_WATER_TEST_MAIN
int main() { TestRadarWater(); }
#endif
