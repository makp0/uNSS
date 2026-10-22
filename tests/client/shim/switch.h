#pragma once
#include <cstdint>
#include <cstddef>
#include <sys/stat.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using Result=uint32_t;
struct AccountUid {u64 uid[2];};
#define R_FAILED(x) ((x)!=0)
#define R_SUCCEEDED(x) ((x)==0)
#define SHA256_HASH_SIZE 32
using Sha256Context=CC_SHA256_CTX;
inline void sha256ContextCreate(Sha256Context*c){CC_SHA256_Init(c);}
inline void sha256ContextUpdate(Sha256Context*c,const void*p,size_t n){CC_SHA256_Update(c,p,n);}
inline void sha256ContextGetHash(Sha256Context*c,void*p){CC_SHA256_Final((unsigned char*)p,c);}
inline Result pmdmntInitialize(){return 0;}
inline Result pmdmntGetApplicationProcessId(u64*p){*p=0;return 1;}
inline void pmdmntExit(){}
inline Result fsdevCommitDevice(const char*){return 0;}
