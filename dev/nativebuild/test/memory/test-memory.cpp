// From AndreikaRPM's fork (dev/test-memory.cpp): the memory bridge and plugin exports against the
// 0.8.0 file bridge as a reference, plus a mailbox stress test. Built by build_pss.sh memory.
// Private host test: real parsers/state plus a fake supported Lua API; no hooks/game.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include "bridge.cpp"
struct lua_State;
using Fn=int(*)(lua_State*);
struct Value { int type=0; std::string text; double number=0; bool boolean=false; Fn fn=nullptr; std::map<std::string,Value> fields; };
struct lua_State { std::vector<Value> stack; bool active=true, room=true; };
#include "plugin.cpp"
bool InstallHooks(){return true;}
void Log(const char*,...){}
namespace ReferenceBridge {void PollCamera(const char*);bool Poll(const char*);void WriteStatus(const char*);}
namespace ReferenceState {void Init(); Settings Snapshot(); int TakeCaptureRequest();void UpdateStatus(void(*)(Status&,void*),void*);}
static unsigned long long checks=0;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int Checkstack(lua_State*L,int){return L->room;}
bool Active(lua_State*L){return L->active;}
int Type(lua_State*L,int i){return L->stack.at(i-1).type;}
const char* ToString(lua_State*L,int i,size_t*n){auto&v=L->stack.at(i-1);*n=v.text.size();return v.text.data();}
void PushBool(lua_State*L,int n){Value v;v.type=1;v.boolean=n!=0;L->stack.push_back(v);}
void PushNum(lua_State*L,double n){Value v;v.type=3;v.number=n;L->stack.push_back(v);}
void PushStr(lua_State*L,const char*s){Value v;v.type=4;v.text=s;L->stack.push_back(v);}
void Table(lua_State*L,int,int){Value v;v.type=5;L->stack.push_back(v);}
void Closure(lua_State*L,Fn f,int){Value v;v.type=6;v.fn=f;L->stack.push_back(v);}
void Field(lua_State*L,int,const char*k){auto v=L->stack.back();L->stack.pop_back();L->stack.back().fields[k]=v;}
const char* missing=nullptr;
void* Resolve(const char*n){if(missing&&!strcmp(missing,n))return nullptr;
#define API(nm,fn) if(!strcmp(n,nm))return reinterpret_cast<void*>(&fn)
API("lua_checkstack",Checkstack);API("lua_type",Type);API("lua_tolstring",ToString);API("lua_pushboolean",PushBool);API("lua_pushnumber",PushNum);API("lua_pushstring",PushStr);API("lua_createtable",Table);API("lua_pushcclosure",Closure);API("lua_setfield",Field);API("is_active_state",Active);
#undef API
return nullptr;}
void Write(const char*p,const std::string&s){std::ofstream f(p,std::ios::binary|std::ios::trunc);f<<s;CHECK(f.good());}
std::string Read(const char*p){std::ifstream f(p,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});}
std::string CameraLine(int count,int frame){std::ostringstream s;for(int i=0;i<count;++i){double v=i==8?frame:(i==0?0.8:(i==1?0.5:(i==30?1:i+0.125)));s<<v<<' ';}if(count==31)s<<"ingame_standard";s<<'\n';return s.str();}
void EqualPose(const CameraPose&a,const CameraPose&b){CHECK(a.valid==b.valid);CHECK(!memcmp(a.pos,b.pos,sizeof(a.pos)));CHECK(!memcmp(a.right,b.right,sizeof(a.right)));CHECK(!memcmp(a.up,b.up,sizeof(a.up)));CHECK(!memcmp(a.fwd,b.fwd,sizeof(a.fwd)));CHECK(!memcmp(a.outRight,b.outRight,sizeof(a.outRight)));CHECK(!memcmp(a.outUp,b.outUp,sizeof(a.outUp)));CHECK(!memcmp(a.outFwd,b.outFwd,sizeof(a.outFwd)));CHECK(!memcmp(a.tan,b.tan,sizeof(a.tan)));CHECK(a.frame==b.frame);CHECK(a.pitch==b.pitch);CHECK(a.roll==b.roll);CHECK(a.aim==b.aim);CHECK(a.luaJitter==b.luaJitter);CHECK(!memcmp(a.jitter,b.jitter,sizeof(a.jitter)));CHECK(!strcmp(a.state,b.state));}
void EqualState(){auto a=State::Snapshot(),b=ReferenceState::Snapshot();CHECK(!memcmp(a.p,b.p,sizeof(a.p)));for(int i=0;i<4;++i)EqualPose(a.recent[i],b.recent[i]);CHECK(a.shaderPath==b.shaderPath);CHECK(a.lutPath==b.lutPath);CHECK(a.dirtPath==b.dirtPath);CHECK(a.shaderGen==b.shaderGen);CHECK(a.lutGen==b.lutGen);CHECK(a.dirtGen==b.dirtGen);CHECK(a.active==b.active);CHECK(State::TakeCaptureRequest()==ReferenceState::TakeCaptureRequest());}
int main(){State::Init();ReferenceState::Init();lua_State L;
// Each missing resolver capability fails closed and explicitly preserves legacy startup.
for(const char*n:{"lua_checkstack","lua_type","lua_tolstring","lua_pushboolean","lua_pushnumber","lua_pushstring","lua_createtable","lua_pushcclosure","lua_setfield","is_active_state"}){missing=n;ResolveApi(Resolve);CHECK(!apiReady);Bridge::ConfigureMemoryCapability(apiReady);CHECK(!Bridge::GetBridgeStatus().pending);CHECK(!Bridge::BeginMemory());CHECK(SuperBLT_Plugin_PushLua(&L)==0);}missing=nullptr;ResolveApi(nullptr);CHECK(!apiReady);ResolveApi(Resolve);CHECK(apiReady);
// Legacy fallback still consumes exact settings/camera and original parser.
Write("dev/test-camera.txt",CameraLine(31,1));Bridge::PollCamera("dev/test-camera.txt");ReferenceBridge::PollCamera("dev/test-camera.txt");EqualState();
Write("dev/test-settings.txt","tick 1\nshader_path A\nlut_path B\ndirt_path C\nend\n");CHECK(Bridge::Poll("dev/test-settings.txt"));CHECK(ReferenceBridge::Poll("dev/test-settings.txt"));EqualState();
Bridge::ConfigureMemoryCapability(true);L.active=false;CHECK(SuperBLT_Plugin_PushLua(&L)==0);CHECK(!Bridge::GetBridgeStatus().pending&&!Bridge::GetBridgeStatus().memory);L.active=true;L.stack.clear();CHECK(SuperBLT_Plugin_PushLua(&L)==1);L.stack.clear();CHECK(Bridge::GetBridgeStatus().pending);auto before=Bridge::GetBridgeStatus();Bridge::PollCamera("dev/test-camera.txt");CHECK(!Bridge::Poll("dev/test-settings.txt"));CHECK(Bridge::GetBridgeStatus().diskCamera==before.diskCamera);CHECK(!Bridge::PublishCamera("x",1));
// Begin cannot overtake either consumer's complete legacy parse/commit section.
for(auto*m:{&g_cameraMutex,&g_settingsMutex}){m->lock();std::atomic<bool>started{false},done{false};std::thread t([&]{started=true;CHECK(Bridge::BeginMemory());done=true;});while(!started)std::this_thread::yield();Sleep(10);CHECK(!done);m->unlock();t.join();CHECK(done);}
CHECK(Bridge::GetBridgeStatus().memory);Bridge::ConfigureMemoryCapability(false);CHECK(Bridge::GetBridgeStatus().memory);
CHECK(SuperBLT_Plugin_PushLua(&L)==1);auto exports=L.stack.back();L.stack.clear();CHECK(exports.fields.at("capability").text=="memory-v1");for(const char*n:{"begin_memory","publish_camera","publish_settings","status_text","bridge_status"})CHECK(exports.fields.at(n).fn!=nullptr);
L.active=false;CHECK(BeginMemory(&L)==0);CHECK(SuperBLT_Plugin_PushLua(&L)==0);CHECK(Bridge::GetBridgeStatus().memory);L.active=true;L.room=false;CHECK(SuperBLT_Plugin_PushLua(&L)==0);L.room=true;
CHECK(BeginMemory(&L)==1&&L.stack.back().boolean);L.stack.clear();
// Differential camera formats/history, bad numbers, truncated final line, extra history.
int frame=2;for(int count:{9,21,30,31})for(int lines=1;lines<=7;++lines){std::string packet;for(int j=0;j<lines;++j)packet+=CameraLine(count,frame++);if(lines%2)packet+="nan nan 0 0 0 0 0 0 9999\ntruncated";Write("dev/test-camera.txt",packet);ReferenceBridge::PollCamera("dev/test-camera.txt");Value arg;arg.type=4;arg.text=packet;L.stack={arg};CHECK(Camera(&L)==1&&L.stack.back().boolean);L.stack.front().text.assign(packet.size(),'Z');Bridge::PollCamera("unused");EqualState();L.stack.clear();}
// Exact original settings parser including commands/clamping/CRLF and invalid end marker.
for(const std::string&s:{"tick 2\nreload 1\ncapture 1\nend\n","tick 3\nreload 2\ncapture 2\nshader_path shaders/new\nlut_path LUT custom\ndirt_path D\nbloom 999999\nunknown 3\nend\n","tick 4\r\nreload 2\r\ncapture 2\r\nend\r\n","tick 999\ninvalid incomplete"}){Write("dev/test-settings.txt",s);bool ref=ReferenceBridge::Poll("dev/test-settings.txt");CHECK(Bridge::PublishSettings(s.data(),s.size()));CHECK(Bridge::Poll("unused")==ref);EqualState();}
// Live status has the original exact text and ongoing temporal permission, not cached file contents.
auto update=[](Status&s,void*){s.jitter="allowed";s.frames=42;s.injectedFrames=38;};State::UpdateStatus(update,nullptr);ReferenceState::UpdateStatus(update,nullptr);char text[32768];Bridge::CopyStatusText(text,sizeof(text));ReferenceBridge::WriteStatus("dev/test-status.txt");auto diskStatus=Read("dev/test-status.txt");for(size_t pos=0;(pos=diskStatus.find("\r\n",pos))!=std::string::npos;)diskStatus.erase(pos,1);CHECK(diskStatus==text);remove("dev/test-status.txt");CHECK(strstr(text,"Camera jitter: allowed"));CHECK(strstr(text,"Frames 42  processed 38"));State::UpdateStatus([](Status&s,void*){s.jitter="not allowed: stale camera";s.injectedFrames=39;},nullptr);CHECK(StatusText(&L)==1);CHECK(L.stack.back().text.find("processed 39")!=std::string::npos);CHECK(L.stack.back().text.find("not allowed: stale camera")!=std::string::npos);L.stack.clear();char tiny[8];CHECK(Bridge::CopyStatusText(tiny,sizeof(tiny))==7&&tiny[7]==0);
// Freshness: only a distinct publication renews the original 500ms lease.
std::string fresh=CameraLine(31,frame++);CHECK(Bridge::PublishCamera(fresh.data(),fresh.size()));Bridge::PollCamera("unused");CHECK(State::Snapshot().p[P_CAMERA_VALID]==1);Sleep(520);Bridge::PollCamera("unused");CHECK(State::Snapshot().p[P_CAMERA_VALID]==0);
CHECK(Bridge::PublishCamera(fresh.data(),fresh.size()));Bridge::PollCamera("unused");CHECK(State::Snapshot().p[P_CAMERA_VALID]==1);Sleep(520);std::string bad="nan nan nan nan nan nan nan nan nan\n";CHECK(Bridge::PublishCamera(bad.data(),bad.size()));Bridge::PollCamera("unused");CHECK(State::Snapshot().p[P_CAMERA_VALID]==0);
// Same valid-history fallback as original parser when the newest line is invalid.
bad=fresh+bad;CHECK(Bridge::PublishCamera(bad.data(),bad.size()));Bridge::PollCamera("unused");CHECK(State::Snapshot().p[P_CAMERA_VALID]==1);
// Bounds/NUL/type rejection retains previously queued valid state.
fresh=CameraLine(31,frame++);CHECK(Bridge::PublishCamera(fresh.data(),fresh.size()));std::string huge(4096,'a');CHECK(!Bridge::PublishCamera(huge.data(),huge.size()));CHECK(!Bridge::PublishCamera("a\0b",3));CHECK(!Bridge::PublishCamera(nullptr,5));CHECK(!Bridge::PublishSettings(huge.data(),65536));L.stack={Value{}};CHECK(Camera(&L)==1&&!L.stack.back().boolean);L.stack.clear();Bridge::PollCamera("unused");CHECK(State::Snapshot().p[P_CAMERA_FRAME]==frame-1);
// No disk access after negotiation; rejected packets never fall back to stale files.
before=Bridge::GetBridgeStatus();for(int i=0;i<50;++i){Bridge::PollCamera("dev/test-camera.txt");Bridge::Poll("dev/test-settings.txt");}auto after=Bridge::GetBridgeStatus();CHECK(after.diskCamera==before.diskCamera&&after.diskSettings==before.diskSettings);CHECK(BridgeStatusLua(&L)==1);CHECK(L.stack.back().fields.at("transport").text=="memory");CHECK(L.stack.back().fields.at("published_camera").number==double(after.publishedCamera));L.stack.clear();
// Multi-producer/single-consumer stress: complete owned strings, monotonic generation, no torn packet.
BoundedMailbox<8> edge;uint64_t edgeSeq=0;char edgeOut[8];size_t edgeSize=0;CHECK(edge.Publish("1234567",7));CHECK(!edge.Publish("12345678",8));CHECK(!edge.Publish("",0));CHECK(!edge.Publish("a\0b",3));CHECK(edge.Take(edgeSeq,edgeOut,edgeSize));CHECK(edgeSize==7&&!strcmp(edgeOut,"1234567"));CHECK(!edge.Take(edgeSeq,edgeOut,edgeSize));
BoundedMailbox<128> box;std::atomic<int> finished{0};std::atomic<unsigned long long> reads{0};std::thread consumer([&]{uint64_t seq=0,last=0;char out[128];size_t size;while(finished<4){if(box.Take(seq,out,size)){CHECK(seq>last);last=seq;int who=-1,number=-1,copy=-2;CHECK(sscanf(out,"%d:%d:%d",&who,&number,&copy)==3);CHECK(who>=0&&who<4&&number==copy);CHECK(size==strlen(out));++reads;}else std::this_thread::yield();}});std::vector<std::thread> producers;for(int j=0;j<4;++j)producers.emplace_back([&,j]{for(int i=0;i<20000;++i){char packet[128];int n=snprintf(packet,sizeof(packet),"%d:%d:%d",j,i,i);if(!box.Publish(packet,n))abort();memset(packet,'X',sizeof(packet));}++finished;});for(auto&t:producers)t.join();consumer.join();CHECK(reads>0);uint64_t finalSeq=0;char finalPacket[128];size_t finalSize=0;CHECK(box.Take(finalSeq,finalPacket,finalSize));CHECK(finalSeq==80000);
// Previously withheld tables are retrievable later without downgrading memory.
lua_State later;later.active=false;CHECK(SuperBLT_Plugin_PushLua(&later)==0);CHECK(Bridge::GetBridgeStatus().memory);
later.active=true;later.room=false;CHECK(SuperBLT_Plugin_PushLua(&later)==0);later.room=true;
CHECK(SuperBLT_Plugin_PushLua(&later)==1);auto recovered=later.stack.back();later.stack.clear();CHECK(recovered.fields.at("begin_memory").fn(&later)==1&&later.stack.back().boolean);later.stack.clear();
Value recoveredSettings;recoveredSettings.type=4;recoveredSettings.text="enabled 0\ntick 5001\nend\n";later.stack={recoveredSettings};
CHECK(recovered.fields.at("publish_settings").fn(&later)==1&&later.stack.back().boolean);CHECK(Bridge::Poll("unused"));CHECK(State::Snapshot().p[FindParam("enabled")]==0);
fresh=CameraLine(31,frame++);Value recoveredCamera;recoveredCamera.type=4;recoveredCamera.text=fresh;later.stack={recoveredCamera};
CHECK(recovered.fields.at("publish_camera").fn(&later)==1&&later.stack.back().boolean);Bridge::PollCamera("unused");CHECK(State::Snapshot().p[P_CAMERA_FRAME]==frame-1);
CHECK(Bridge::GetBridgeStatus().diskCamera==after.diskCamera&&Bridge::GetBridgeStatus().diskSettings==after.diskSettings);
remove("dev/test-camera.txt");remove("dev/test-settings.txt");printf("PASS %llu assertions; %llu concurrent packets checked\n",checks,reads.load());return 0;}
