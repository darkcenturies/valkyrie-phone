#pragma once
#include <string>
#include <map>
#include <istream>
#include <ostream>
#include <iomanip>
namespace ActionKeys {
struct Chord {int key=0,mods=0;bool operator==(const Chord& b)const{return key==b.key&&mods==b.mods;}};
inline bool Reserved(Chord c){return c.key<8 || c.key>254 || c.key==27 || (c.key>=16&&c.key<=18) || (c.key>=91&&c.key<=92) || (c.key>=160&&c.key<=165) || (c.key>=116&&c.key<=120) || (c.key==90&&(c.mods&4)) || (c.key==115&&(c.mods&4)) || c.mods<0 || c.mods>7;}
struct Store {
 std::map<std::string,Chord> bindings;
 std::string Assign(const std::string& id,Chord c){if(Reserved(c))return "That key is reserved";for(auto& [other,chord]:bindings)if(other!=id&&chord==c)return "Already assigned to "+other;bindings[id]=c;return {};}
 void Load(std::istream& in){bindings.clear();std::string id;Chord c;while(in>>std::quoted(id)>>c.key>>c.mods)Assign(id,c);}
 void Save(std::ostream& out)const{for(auto& [id,c]:bindings)out<<std::quoted(id)<<' '<<c.key<<' '<<c.mods<<'\n';}
};
inline bool Rising(bool now,bool previous,bool allowed){return now&&!previous&&allowed;}
// Hold a modest forward axis only while eligible. Manual movement always wins.
inline short WalkAxis(short original,bool enabled,bool eligible){return enabled&&eligible&&original==0?-64:original;}
}
