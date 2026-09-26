#pragma once
#include <algorithm>
#include <istream>
#include <sstream>
#include <string>
#include <vector>
namespace WardrobeData {
struct Item {std::string texture,label,model;int slot=0;};
inline bool SafeName(const std::string& s){return !s.empty()&&s.size()<32&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';});}
inline std::vector<Item> Read(std::istream& file){
    std::vector<Item> out;std::string line;bool clothes=false;
    while(std::getline(file,line)){
        line.resize(line.find('#')==std::string::npos?line.size():line.find('#'));std::replace(line.begin(),line.end(),',',' ');
        std::istringstream row(line);std::string first;row>>first;
        if(first=="section"){std::string section;row>>section;clothes=section=="Clothes"||section=="clothes";continue;}
        if(first=="end"){clothes=false;continue;}
        if(!clothes||first.empty())continue;
        Item item;item.texture=first;if(!(row>>item.label>>item.model>>item.slot))continue;
        if(!SafeName(item.texture)||!SafeName(item.model)||!SafeName(item.label))continue;
        if(item.slot!=0&&item.slot!=2&&item.slot!=3&&(item.slot<13||item.slot>17))continue;
        if(out.size()<4096)out.push_back(std::move(item));
    }
    return out;
}
}
