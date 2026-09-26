#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <vector>
#include <algorithm>
#include "imgui.h"
#include "TrainerUI.h"
#include "Render.h"
#include "ActionKeys.h"
#include "ActionBindings.h"
namespace ActionBindings {
namespace {
struct Action{std::string label,group;std::function<void()> run;std::function<bool()> state;};std::vector<Action> actions;
ActionKeys::Store store;std::string capture,error;bool armed=false;bool previous[256]{};char filter[96]{};
std::string Path(){char p[MAX_PATH]{};GetModuleFileNameA(nullptr,p,MAX_PATH);std::string s=p;s.resize(s.find_last_of("\\/")+1);return s+"valkyrie-actions.cfg";}
void Save(){std::ofstream out(Path());store.Save(out);}
std::string Name(ActionKeys::Chord c){std::string s;if(c.mods&1)s+="Ctrl + ";if(c.mods&2)s+="Shift + ";if(c.mods&4)s+="Alt + ";char text[64]{};UINT scan=MapVirtualKeyA(c.key,MAPVK_VK_TO_VSC);if(c.key==VK_LEFT||c.key==VK_RIGHT||c.key==VK_UP||c.key==VK_DOWN||c.key==VK_INSERT||c.key==VK_DELETE||c.key==VK_HOME||c.key==VK_END||c.key==VK_PRIOR||c.key==VK_NEXT)scan|=0x100;GetKeyNameTextA(scan<<16,text,64);return s+(text[0]?text:std::to_string(c.key));}
void Capture(const std::string& label){capture=label;armed=false;error.clear();}
}
bool Capturing(){return !capture.empty();}
bool Focused(){DWORD pid=0;HWND window=GetForegroundWindow();GetWindowThreadProcessId(window,&pid);return pid==GetCurrentProcessId()&&!IsIconic(window);}
bool Paused(){return *reinterpret_cast<bool*>(0xB7CB48)||*reinterpret_cast<bool*>(0xB7CB49);}
void Register(const char* label,const char* group,std::function<void()> action,std::function<bool()> state){for(auto& a:actions)if(a.label==label)return;actions.push_back({label,group,std::move(action),std::move(state)});}
int Count(){return static_cast<int>(actions.size());}
const char* Label(int i){return i>=0&&i<Count()?actions[i].label.c_str():"";}
const char* Group(int i){return i>=0&&i<Count()?actions[i].group.c_str():"";}
void Run(int i){if(i>=0&&i<Count())actions[i].run();}
int State(int i){if(i<0||i>=Count()||!actions[i].state)return -1;return actions[i].state()?1:0;}
void Load(){std::ifstream in(Path());store.Load(in);}
void Context(const char* label){
 auto found=std::find_if(actions.begin(),actions.end(),[&](auto& a){return a.label==label;});if(found==actions.end())return;
 if(ImGui::BeginPopupContextItem()){
  ImGui::TextUnformatted(label);
  auto binding=store.bindings.find(label);if(binding!=store.bindings.end())ImGui::TextDisabled("%s",Name(binding->second).c_str());
  if(ImGui::MenuItem("Bind key"))Capture(label);
  if(binding!=store.bindings.end()&&ImGui::MenuItem("Unbind")){store.bindings.erase(label);Save();}
  ImGui::EndPopup();
 }
}
void Update(){
 bool now[256]{};for(int k=0;k<256;++k)now[k]=(GetAsyncKeyState(k)&0x8000)!=0;
 const int mods=(now[VK_CONTROL]?1:0)|(now[VK_SHIFT]?2:0)|(now[VK_MENU]?4:0);
 bool typing=ImGui::GetCurrentContext()&&ImGui::GetIO().WantTextInput;
 const bool focus=Focused();
 if(!capture.empty()){
  if(!focus||!Render::IsMenuOpen()){capture.clear();error.clear();}
  else if(!armed){bool any=false;for(int k=1;k<256;++k)any|=now[k];armed=!any;}
  else for(int k=8;k<256;++k)if(now[k]&&!previous[k]){
   if(k==VK_ESCAPE){capture.clear();error.clear();break;}
   if(k==VK_SHIFT||k==VK_CONTROL||k==VK_MENU||(k>=VK_LSHIFT&&k<=VK_RMENU))continue;
   error=store.Assign(capture,{k,mods});if(error.empty()){Save();capture.clear();}break;
  }
 }else if(focus&&!Paused()&&!typing){
  for(auto& a:actions){auto it=store.bindings.find(a.label);if(it!=store.bindings.end()){auto c=it->second;if(ActionKeys::Rising(now[c.key],previous[c.key],mods==c.mods)){a.run();break;}}}
 }
 std::copy(std::begin(now),std::end(now),previous);
}
void Prompt(){
 if(capture.empty())return;
 ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x*.5f,ImGui::GetIO().DisplaySize.y*.5f),ImGuiCond_Always,{.5f,.5f});
 ImGui::SetNextWindowBgAlpha(.98f);
 if(ImGui::Begin("Bind action",nullptr,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings)){
  TrainerUI::Heading(capture.c_str(),29);ImGui::TextUnformatted("Press a key combination");
  TrainerUI::StatusLine(error.c_str(),2);
  if(TrainerUI::SmallButton("Cancel")){capture.clear();error.clear();}
 }
 ImGui::End();
}
void Draw(){
 ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##action-search","Find an action",filter,sizeof(filter));
 std::string query=filter;std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c){return (char)tolower(c);});
 ImGui::TextWrapped("Choose Bind key, then press a key or combination. Escape cancels.");
 int matches=0;
 if(ImGui::BeginTable("##action-bindings",3,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_RowBg)){
 ImGui::TableSetupColumn("Action",ImGuiTableColumnFlags_WidthStretch,1.5f);ImGui::TableSetupColumn("Shortcut",ImGuiTableColumnFlags_WidthStretch,1.f);ImGui::TableSetupColumn("",ImGuiTableColumnFlags_WidthFixed,85);ImGui::TableHeadersRow();
 for(auto& a:actions){std::string match=a.group+" "+a.label;std::transform(match.begin(),match.end(),match.begin(),[](unsigned char c){return (char)tolower(c);});if(!query.empty()&&match.find(query)==std::string::npos)continue;
  ++matches;ImGui::PushID(a.label.c_str());ImGui::TableNextRow();ImGui::TableNextColumn();
  ImGui::TextWrapped("%s",a.label.c_str());ImGui::TextDisabled("%s",a.group.c_str());ImGui::TableNextColumn();
  auto binding=store.bindings.find(a.label);std::string key=binding==store.bindings.end()?"Bind key":Name(binding->second);
  if(TrainerUI::SmallButton(key.c_str()))Capture(a.label);
  ImGui::TableNextColumn();if(binding!=store.bindings.end()&&TrainerUI::SmallButton("Unbind")){store.bindings.erase(a.label);Save();}
  ImGui::PopID();
 }
 ImGui::EndTable();
 }
 if(!matches)ImGui::TextWrapped("No matching actions. Try another name or category.");

}
}
