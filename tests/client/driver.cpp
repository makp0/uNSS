#include "fingerprint.hpp"
#include "savedata.hpp"
#include "fileio.hpp"
#include "sync.hpp"
#include "zipio.hpp"
#include "title.hpp"
#include "http.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cassert>
#include <sys/time.h>
#include <sys/resource.h>
#include <csignal>
namespace fs=std::filesystem;
std::string account="profile",bcat="bcat-data";
std::vector<u64> targetTitles={1};
bool bcatError=false,missing=false,failInstall=false;
int testRename(const char*oldName,const char*newName){if(fs::exists(newName)){errno=EEXIST;return -1;}if(failInstall && std::string(oldName)=="candidate"){errno=EIO;return -1;}return ::rename(oldName,newName);}
void put(const std::string&p,const std::string&data){fs::create_directories(fs::path(p).parent_path());std::ofstream f(p,std::ios::binary);f<<data;}
std::string read(const std::string&p){std::ifstream f(p,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});}
int mountSaveData(const std::string&p,AccountUid,u64){if(missing)return MOUNT_TARGET_NOT_FOUND;fs::remove(p+":");fs::create_directory_symlink(fs::absolute(account),p+":");return 0;}
int mountBcatSaveData(const std::string&p,u64){if(bcatError)return -1;if(!fs::exists(bcat))return MOUNT_TARGET_NOT_FOUND;fs::remove(p+":");fs::create_directory_symlink(fs::absolute(bcat),p+":");return 0;}
int unmount(const std::string&p){fs::remove(p+":");return 0;}
int createSaveData(AccountUid,u64){return 0;}
int getTitleName(u64,std::string&s){s="Fixture";return 0;}
int probeAllTitles(AccountUid,std::vector<u64>&v){v=targetTitles;return 0;}
int probeSaveDataCreatedTitles(AccountUid u,std::vector<u64>&v){return probeAllTitles(u,v);}
void filterExcludedTitles(std::vector<u64>&,const std::string&,const std::string&){}
int main(int argc,char**argv){
 if(argc==2){fs::create_directories("profile");fs::create_directories("bcat-data");fs::create_directories("cache:");fs::copy_file(argv[1],"cache:/0000000000000001.sar");return restoreSaveData({},1,"cache:")==0?0:1;}
 assert(argc==3);std::string fixtures=argv[1],url=argv[2];
 put("replacement/original","old");put("replacement/next","new");assert(replaceCacheFile("replacement/next","replacement/original"));assert(read("replacement/original")=="new");
 {std::ofstream f("candidate");f<<"fail";}failInstall=true;assert(!replaceCacheFile("candidate","replacement/original"));assert(read("replacement/original")=="new");failInstall=false;assert(replaceCacheFile("candidate","replacement/original"));
 fs::rename("replacement/original","replacement/original.previous");put("replacement/next","recovered");assert(replaceCacheFile("replacement/next","replacement/original"));assert(read("replacement/original")=="recovered");
 fs::create_directories("profile/empty");put("profile/state","AAAA");put("bcat-data/news","news1");fs::create_directories("cache:");
 std::string original,changed,again;assert(fingerprintTree(account,original));assert(original=="6fde58824f18b867a2ddf84852fee23a4f464b4dfa56b7507bee846c1fe236a0");assert(fingerprintTree(account,again)&&again==original);
 struct stat st;stat("profile/state",&st);put("profile/state","BBBB");timeval tv[2]={{st.st_atime,0},{st.st_mtime,0}};utimes("profile/state",tv);assert(fingerprintTree(account,changed)&&changed!=original);
 put("profile/state","AAAA");assert(fingerprintTree(account,again)&&again==original);
 fs::rename("profile/state","profile/renamed");assert(fingerprintTree(account,changed)&&changed!=original);fs::rename("profile/renamed","profile/state");
 fs::remove("profile/empty");assert(fingerprintTree(account,changed)&&changed!=original);fs::create_directories("profile/empty");
 fs::create_symlink("state","profile/link");assert(!fingerprintTree(account,changed));fs::remove("profile/link");
 assert(!fingerprintTree("missing",changed));
 assert(archiveSaveData({},1,"cache:")==0);ZipReader valid;assert(valid.open("cache:/0000000000000001.sar")&&valid.validate());fs::create_directories("extracted:");assert(valid.extractAll("extracted:"));assert(read("extracted:/saves/state")=="AAAA"&&read("extracted:/bcat/news")=="news1"&&fs::is_directory("extracted:/saves/empty"));assert(!valid.extractAll("/dev/null/unss"));valid.close();
 put("profile/state","changed");put("profile/extra","extra");put("bcat-data/news","changed");assert(restoreSaveData({},1,"cache:")==0);
 assert(read("profile/state")=="AAAA"&&!fs::exists("profile/extra")&&fs::is_directory("profile/empty")&&read("bcat-data/news")=="news1");
 for(const auto& name:{"corrupt.zip","traversal.zip","duplicate.zip","absolute.zip","invalid.zip"}){
  fs::copy_file(fixtures+"/"+name,"cache:/0000000000000001.sar",fs::copy_options::overwrite_existing);
  put("profile/state","KEEP");assert(restoreSaveData({},1,"cache:")!=0);assert(read("profile/state")=="KEEP");
 }
 ZipReader no;assert(!no.open("not-there.zip"));ZipWriter bad;assert(!bad.open("missing/path/archive"));
 {ZipWriter full;assert(full.open("size-limited.zip"));struct rlimit originalLimit;assert(getrlimit(RLIMIT_FSIZE,&originalLimit)==0);auto limit=originalLimit;limit.rlim_cur=0;signal(SIGXFSZ,SIG_IGN);assert(setrlimit(RLIMIT_FSIZE,&limit)==0);assert(!full.close());assert(setrlimit(RLIMIT_FSIZE,&originalLimit)==0);}
 assert(archiveSaveData({},1,"cache:")==0);auto archive=read("cache:/0000000000000001.sar");bcatError=true;assert(archiveSaveData({},1,"cache:")!=0);assert(read("cache:/0000000000000001.sar")==archive);bcatError=false;
 SyncOptions options;options.uid={1,2};options.nickname="fixture";options.serverUrl=url;options.saveDataPath="cache:/profile";options.remoteEnabled=true;std::vector<std::string> logs;
 auto log=[&](const std::string&s){logs.push_back(s);};
 assert(pushAllSaves(options,log)==0);assert(!hasSaveDataChanged(options,1));
 logs.clear();assert(pushAllSaves(options,log)==0);bool skipped=false;for(auto&s:logs)if(s.find("Skipping 1 unchanged")!=std::string::npos)skipped=true;assert(skipped);
 put("profile/state","DIFF");utimes("profile/state",tv);assert(hasSaveDataChanged(options,1));assert(pushAllSaves(options,log)==0);assert(!hasSaveDataChanged(options,1));
 put("bcat-data/news","news2");assert(hasSaveDataChanged(options,1));assert(pushAllSaves(options,log)==0);assert(!hasSaveDataChanged(options,1));
 fs::remove("profile/state");assert(hasSaveDataChanged(options,1));assert(pushAllSaves(options,log)==0);
 put("profile/state","retry");options.serverUrl=url+"/fail";assert(pushAllSaves(options,log)!=0);assert(hasSaveDataChanged(options,1));
 options.serverUrl=url+"/race";assert(pushAllSaves(options,log)==0);assert(hasSaveDataChanged(options,1));
 bool progress=false;for(auto&s:logs)if(s.find("Uploading")!=std::string::npos&&s.find("KiB/s")!=std::string::npos)progress=true;assert(progress);
 options.serverUrl=url+"/corrupt";put("profile/state","KEEP");const auto cacheBefore=read("cache:/profile/0000000000000001.sar");assert(pullAllSaves(options,log)!=0);assert(read("profile/state")=="KEEP");assert(read("cache:/profile/0000000000000001.sar")==cacheBefore);
 options.serverUrl=url+"/fail";options.stopOnUploadError=true;targetTitles={1,2};logs.clear();assert(pushAllSaves(options,log)<0);int errors=0;for(auto&s:logs){if(s.find("Failed to push")!=std::string::npos)++errors;assert(s.find("[002/")==std::string::npos);}assert(errors==1);
 options.stopOnUploadError=false;logs.clear();assert(pushAllSaves(options,log)<0);errors=0;for(auto&s:logs)if(s.find("Failed to push")!=std::string::npos)++errors;assert(errors==2);
 HTTPClient badUrl;badUrl.setUrl("http://");assert(badUrl.perform()==HTTPCLIENT_ERROR_CURL_URL);assert(badUrl.getLastCurlCode()==CURLE_URL_MALFORMAT);
 options.serverUrl="http://[";options.stopOnUploadError=true;logs.clear();assert(pushAllSaves(options,log)<0);bool raw=false;for(auto&s:logs)if(s.find("curl=3")!=std::string::npos)raw=true;assert(raw);
 targetTitles={1};missing=true;assert(!hasSaveDataChanged(options,1));missing=false;
 std::cout<<"PASS: SHA-256 detects same-time/same-size edits, renames, deletions, empty directories and BCAT changes; failed and raced uploads remain dirty; progress emitted; real archive/restore code restores sandbox bytes and rejects corrupt/unsafe archives before touching data; failures preserve old archive\n";
}
