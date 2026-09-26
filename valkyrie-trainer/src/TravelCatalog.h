#pragma once
#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace TravelCatalog {
struct Zone {std::string key,id;float x,y,z,minX,minY,maxX,maxY;};
inline std::string Trim(std::string s){auto a=s.find_first_not_of(" \t\r\n");if(a==s.npos)return {};return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
inline std::vector<Zone> Read(std::istream& input){
    std::vector<Zone> result;std::string line;
    while(std::getline(input,line)){
        line=line.substr(0,line.find('#'));std::istringstream row(line);std::string part;std::vector<std::string> fields;
        while(std::getline(row,part,','))fields.push_back(Trim(part));
        if(fields.size()!=10||fields[0].empty()||fields[9].empty()||fields[9].size()>7)continue;
        float n[8]{};bool valid=true;
        for(int i=0;i<8;++i){std::istringstream number(fields[i+1]);if(!(number>>n[i])||!std::isfinite(n[i])||std::fabs(n[i])>=200000)valid=false;number>>std::ws;if(!number.eof())valid=false;}
        if(!valid||n[1]>=n[4]||n[2]>=n[5]||n[3]>n[6])continue;
        std::string key=fields[9];
        if(n[0]==3){
            const char* cities[]={"", "Los Santos", "San Fierro", "Las Venturas", "Desert", "Liberty City / New York", "Vice City / Miami", "Minwall", "Carcer City", "Cheyenne", "Philadelphia", "Jackalope", "Cottonmouth", "Upstate Liberty", "Bullworth", "Minooka County"};
            int city=int(n[7]);if(city<1||city>=int(sizeof cities/sizeof *cities))continue;key=std::string("City: ")+cities[city];
        }else if(n[0]!=0||key=="UNUSED")continue;
        result.push_back({key,fields[0],(n[1]+n[4])*.5f,(n[2]+n[5])*.5f,std::clamp(n[6]+100.f,1000.f,5000.f),n[1],n[2],n[4],n[5]});
    }return result;
}
inline std::vector<Zone> Load(const std::string& root){
    std::ifstream dat(root+"data/gta.dat");std::string line;std::vector<Zone> result;
    while(std::getline(dat,line)){
        line=Trim(line.substr(0,line.find('#')));if(line.size()<5||line.substr(0,4)!="IPL ")continue;
        auto path=Trim(line.substr(4));auto lower=path;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(lower.size()<4||lower.substr(lower.size()-4)!=".zon"||path.find("..")!=path.npos||path.find(':')!=path.npos)continue;
        std::ifstream file(root+path);auto zones=Read(file);result.insert(result.end(),zones.begin(),zones.end());
    }return result;
}
}
