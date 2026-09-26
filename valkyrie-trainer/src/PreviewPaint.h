#pragma once
#include <array>
#include <algorithm>
#include <istream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace PreviewPaint {
using Colors = std::array<unsigned,4>;
struct Data {
    std::vector<std::array<unsigned char,3>> palette;
    std::map<std::string,std::vector<Colors>> vehicles;
};
inline Data Read(std::istream& input) {
    Data data; std::string section,line;
    while(std::getline(input,line)) {
        line=line.substr(0,line.find('#'));
        std::replace(line.begin(),line.end(),',',' ');
        std::istringstream row(line);std::string first;
        if(!(row>>first))continue;
        if(first=="col" || first=="car" || first=="car4") {section=first;continue;}
        if(first=="end") {section.clear();continue;}
        if(section=="col") {
            std::istringstream rgb(line);int r,g,b;
            if(rgb>>r>>g>>b && r>=0 && r<=255 && g>=0 && g<=255 && b>=0 && b<=255)
                data.palette.push_back({(unsigned char)r,(unsigned char)g,(unsigned char)b});
        } else if(section=="car" || section=="car4") {
            std::vector<unsigned> values;unsigned v;
            while(row>>v)values.push_back(v);
            const size_t stride=section=="car4"?4:2;
            auto& choices=data.vehicles[first];
            for(size_t i=0;i+stride<=values.size();i+=stride) {
                Colors colors{};std::copy_n(values.begin()+i,stride,colors.begin());
                if(std::find(choices.begin(),choices.end(),colors)==choices.end())choices.push_back(colors);
            }
        }
    }
    return data;
}
}
