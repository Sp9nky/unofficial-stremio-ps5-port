#include "torrent/engine.h"
#include <cassert>
#include <cstdio>
using namespace bt;
int main(){
  std::vector<FileInfo> f={{"Show/S01E01.mkv",500,0},{"Show/S01E02.1080p.mkv",900,500},{"Show/S01E02.720p.mkv",300,1400},{"Show/readme.txt",9999,1700}};
  assert(Engine::guess_file(f,1,2)==1);
  assert(Engine::guess_file(f,1,2,{"720p"})==2);          // must include wins over the largest
  assert(Engine::guess_file(f,-1,-1,{"e01"})==0);
  assert(Engine::guess_file(f,-1,-1,{"nomatch"})==1);     // nothing matches: ignored
  assert(Engine::guess_file(f,-1,-1,{"("})==1);           // bad regex: ignored
  assert(Engine::guess_file(f,-1,-1,{"S01","720P"})==2);  // all must match, case-insensitive
  puts("guess_file OK");
}
