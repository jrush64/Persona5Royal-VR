// Read-only scene membership/visibility comparison. No scene flags are modified.
namespace SceneVisibility {
struct Entry {uintptr_t node=0;uint32_t flags=0;float visibility=0;bool geometry=false;};
bool CullChanged(const Entry& before,const Entry& after){
 return before.geometry&&after.geometry&&before.node==after.node&&((before.flags^after.flags)&0x4000)!=0;
}
struct Scan {std::unordered_map<uintptr_t,Entry> entries;unsigned nodes=0;bool capped=false;};
Scan Read(uintptr_t root){
 Scan out;std::vector<uintptr_t> todo{root};std::unordered_set<uintptr_t> seen;
 while(!todo.empty()&&seen.size()<16384){
  auto node=todo.back();todo.pop_back();if(!seen.insert(node).second)continue;
  float m[16],visibility=0;
  uint32_t nodeKind=0;
  if(!Rd(node,nodeKind)||nodeKind!=3||!SafeRead((void*)(node+0x20),m,sizeof(m))||
     !Rd(node+0x108,visibility)||!std::isfinite(visibility))continue;
  bool affine=fabsf(m[15]-1)<.01f && fabsf(m[3])+fabsf(m[7])+fabsf(m[11])<.01f;
  for(float v:m)if(!std::isfinite(v)||fabsf(v)>1e8f)affine=false;
  if(!affine)continue;
  ++out.nodes;out.entries[node]={node,0,visibility,false};
  uintptr_t child=0,next=0,object=0;Rd(node+0xd8,child);Rd(node+0xe0,next);
  if(child)todo.push_back(child);if(node!=root&&next)todo.push_back(next);
  Rd(node+0x110,object);std::unordered_set<uintptr_t> objects;
  while(object&&objects.size()<64&&out.entries.size()<32768){
   if(!objects.insert(object).second)break;
   uintptr_t owner=0;uint32_t kind=0,flags=0;
   if(!Rd(object,kind)||!Rd(object+8,owner)||owner!=node)break;
   if(kind==4&&Rd(object+0x20,flags))out.entries[object]={node,flags,visibility,true};
   if(!Rd(object+0x18,object))break;
  }
  if(objects.size()==64||out.entries.size()>=32768){out.capped=true;break;}
 }
 if(!todo.empty())out.capped=true;return out;
}
std::unordered_map<uintptr_t,Entry> previous;
uintptr_t previousRoot=0;ULONGLONG nextSample=0;unsigned samples=0;bool active=false;
void Tick(){
 if(GfdCam::sceneCaptureRequested.exchange(false)){
  previous.clear();previousRoot=0;nextSample=0;samples=0;active=true;
  GfdCam::sceneCaptureStatus=1;
 }
 if(!active||GetTickCount64()<nextSample)return;
 nextSample=GetTickCount64()+1000;
 uintptr_t camera=0,player=0,scene=0,root=0;
 if(!FieldIdentity(g_contextTask.load(),camera,player))return;
 for(int i=0;i<2;++i){uintptr_t s=0,c=0;
  if(ProbeRd(g_global.load()+0x60+i*8,s)&&ProbeRd(s+0x10,c)&&c==camera){scene=s;break;}}
 float view[16];
 if(!scene||!ProbeRd(scene+0x50,root)||!ProbeRead(camera+0x20,view,sizeof(view)))return;
 auto now=Read(root);++samples;
 if(!now.nodes){if(g_log)g_log("[scene-visibility] sample=%u invalid scene root=%p",samples,(void*)root);}
 else {
  unsigned added=0,removed=0,changed=0,reported=0;bool selected=false;
  const bool baseline=root!=previousRoot;
  for(const auto& pair:now.entries){auto it=previous.find(pair.first);
   bool isAdded=it==previous.end();bool differs=!isAdded&&(it->second.flags!=pair.second.flags||it->second.visibility!=pair.second.visibility||it->second.node!=pair.second.node);
   if(isAdded)++added;if(differs)++changed;
   if(!baseline&&!isAdded&&!selected&&CullChanged(it->second,pair.second)){
    char name[80]={};ProbeName(pair.second.node+0xf0,name);
    if(strstr(name,"build")||strstr(name,"wall")||strstr(name,"Box")){
     selected=true;CullTrace::flagOwner=pair.second.node;CullTrace::flagObject=pair.first;
     if(g_log)g_log("[cull-writer] observed transition object=%p owner=%p name=%s flags=%X->%X",(void*)pair.first,(void*)pair.second.node,name,it->second.flags,pair.second.flags);
     CullTrace::Start();
    }
   }
   if(!baseline&&(isAdded||differs)&&reported++<48&&g_log){
    char name[80]={};ProbeName(pair.second.node+0xf0,name);float m[16]={};NodeMatrix(pair.second.node,m);
    g_log("[scene-visibility] %s object=%p node=%p geometry=%d name=%s flags=%X->%X visibility=%.3f->%.3f xyz=(%.1f %.1f %.1f)",isAdded?"added":"changed",(void*)pair.first,(void*)pair.second.node,pair.second.geometry,name,isAdded?0:it->second.flags,pair.second.flags,isAdded?0:it->second.visibility,pair.second.visibility,m[12],m[13],m[14]);
   }
  }
  for(const auto& pair:previous)if(now.entries.find(pair.first)==now.entries.end()){
   ++removed;if(!baseline&&reported++<48&&g_log)g_log("[scene-visibility] removed object=%p node=%p",(void*)pair.first,(void*)pair.second.node);
  }
  if(g_log)g_log("[scene-visibility] sample=%u scene=%p root=%p baseline=%d nodes=%u entries=%u capped=%d added=%u removed=%u changed=%u forward=(%.3f %.3f %.3f)",samples,(void*)scene,(void*)root,baseline,now.nodes,(unsigned)now.entries.size(),now.capped,added,removed,changed,-view[2],-view[6],-view[10]);
  previous=std::move(now.entries);previousRoot=root;
 }
 if(samples>=60){active=false;previous.clear();GfdCam::sceneCaptureStatus=2;if(g_log)g_log("[scene-visibility] complete");}
}
}
