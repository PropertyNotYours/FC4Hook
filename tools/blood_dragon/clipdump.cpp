// clipdump <clip file>: a clip's nested gun animations as JSON (for overkill_clips.py)
//   build: c++ -std=c++17 -I../../src -o clipdump clipdump.cpp
#include "../../src/aniclip.cpp"
#include <fstream>
#include <iterator>
using namespace fc4; using namespace fc4::ani;
// nested gun clips of a clip file as JSON: [{bone, frames, length, tracks:{hash:{r:[[x,y,z,w]..], p:[[x,y,z]..]}}}]
int main(int c,char**v){std::ifstream f(v[1],std::ios::binary);Bytes d{std::istreambuf_iterator<char>(f),{}};
 Nested ns; printf("["); if(ParseNested(d,ns)) for(size_t k=0;k<ns.e.size();++k){Clip cl;std::string e; if(!Decode(NestedAsFile(d,ns.e[k]),cl,&e)) continue;
  printf("%s{\"bone\":\"%s\",\"frames\":%d,\"tracks\":{",k?",":"",ns.e[k].bone.c_str(),cl.frames);
  for(int t=0;t<cl.tracks;++t){printf("%s\"%08x\":{\"r\":[",t?",":"",cl.hashes[t]); for(size_t i=0;i<cl.rot[t].size();++i){auto q=cl.rot[t][i];printf("%s[%.7g,%.7g,%.7g,%.7g]",i?",":"",q.x,q.y,q.z,q.w);} printf("],\"p\":[");
   for(size_t i=0;i<cl.pos[t].size();++i){auto p=cl.pos[t][i];printf("%s[%.7g,%.7g,%.7g]",i?",":"",p.x,p.y,p.z);} printf("]}");}
  printf("}}");}
 printf("]\n");}
