#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

// Read the game's processed water.dat; retain triangle footprints and heights.
// A global sea-level sheet floods dry terrain that happens to lie below it.
namespace radar_water {
struct Point { float x, y, z; };
using Triangle = std::array<Point, 3>;
inline std::vector<Triangle> Read(std::istream& input) {
    std::vector<Triangle> out;
    std::string line;
    while (std::getline(input, line)) {
        line = line.substr(0, line.find_first_of("#;"));
        std::istringstream row(line);
        std::vector<float> values;
        float value;
        while (row >> value) values.push_back(value);
        if (!row.eof() || (values.size() != 22 && values.size() != 29)) continue;
        if (!std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); })) continue;
        Point p[4]{};
        const size_t count = (values.size() - 1) / 7;
        bool valid = true;
        for (size_t i = 0; i < count; ++i) {
            p[i] = {values[i*7], values[i*7+1], values[i*7+2]};
            if (std::abs(p[i].x) > 20000.f || std::abs(p[i].y) > 20000.f || std::abs(p[i].z) > 5000.f) valid = false;
        }
        if (!valid) continue;
        out.push_back({p[0], p[1], p[2]});
        if (count == 4) out.push_back({p[1], p[3], p[2]});
    }
    return out;
}
inline Point Mix(Point a, Point b, float t) {
    return {a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t};
}
inline float Axis(Point p, unsigned axis) { return axis ? p.y : p.x; }
inline std::vector<Point> Clip(const std::vector<Point>& polygon, unsigned axis, float edge, bool greater) {
    std::vector<Point> out;
    if (polygon.empty()) return out;
    Point previous = polygon.back();
    bool wasInside = greater ? Axis(previous,axis) >= edge : Axis(previous,axis) <= edge;
    for (Point current : polygon) {
        bool inside = greater ? Axis(current,axis) >= edge : Axis(current,axis) <= edge;
        if (inside != wasInside) {
            float t = (edge-Axis(previous,axis))/(Axis(current,axis)-Axis(previous,axis));
            out.push_back(Mix(previous,current,t));
        }
        if (inside) out.push_back(current);
        previous = current; wasInside = inside;
    }
    return out;
}
inline float DistanceSquared(Point a, Point b) {
    return (a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z);
}
inline void Subdivide(Triangle t, unsigned depth, std::vector<Point>& out) {
    unsigned edge = 0;
    float longest = 0;
    for (unsigned i=0; i<3; ++i) {
        float length = DistanceSquared(t[i], t[(i+1)%3]);
        if (length > longest) { longest=length; edge=i; }
    }
    // Nearby vertices preserve per-vertex fog on large ocean polygons.
    if (longest <= 32.f*32.f || depth == 16) {
        out.insert(out.end(), t.begin(), t.end()); return;
    }
    unsigned b=(edge+1)%3, c=(edge+2)%3;
    Point middle=Mix(t[edge],t[b],.5f);
    Subdivide({t[edge],middle,t[c]},depth+1,out);
    Subdivide({middle,t[b],t[c]},depth+1,out);
}
inline std::vector<Point> Mesh(const std::vector<Triangle>& patches, float x, float y, float extent) {
    std::vector<Point> out;
    for (const auto& patch : patches) {
        std::vector<Point> polygon(patch.begin(),patch.end());
        polygon=Clip(polygon,0,x-extent,true);
        polygon=Clip(polygon,0,x+extent,false);
        polygon=Clip(polygon,1,y-extent,true);
        polygon=Clip(polygon,1,y+extent,false);
        for (size_t i=1; i+1<polygon.size(); ++i) {
            Triangle t{polygon[0],polygon[i],polygon[i+1]};
            float area=(t[1].x-t[0].x)*(t[2].y-t[0].y)-(t[1].y-t[0].y)*(t[2].x-t[0].x);
            if (std::abs(area) > .0001f) Subdivide(t,0,out);
        }
    }
    return out;
}
} // namespace radar_water
