from pathlib import Path
import subprocess,tempfile,sys
root=Path(sys.argv[1]).resolve()/'client/source'
with tempfile.TemporaryDirectory() as td:
 p=Path(td)
 (p/'switch.h').write_text('''#include <cstdint>
using u32=uint32_t;using Result=u32;struct SslConnection { int id; };
#define R_FAILED(x) ((x)!=0)
#define R_VALUE(x) ((x)&0x3fffff)
#define MAKERESULT(m,d) ((m)|((d)<<9))
''')
 (p/'test.cpp').write_text('''#include <switch.h>
#include "tlsdiag.hpp"
#include <cassert>
#include <cstring>
Result result; SslConnection connection{42};char buffer[8];
extern "C" Result __real_sslConnectionRead(SslConnection* c,void* b,u32 n,u32* out){assert(c==&connection&&b==buffer&&n==8);*out=3;return result;}
extern "C" Result __real_sslConnectionWrite(SslConnection* c,const void* b,u32 n,u32* out){assert(c==&connection&&b==buffer&&n==8);*out=4;return result;}
extern "C" Result __wrap_sslConnectionRead(SslConnection*,void*,u32,u32*);
extern "C" Result __wrap_sslConnectionWrite(SslConnection*,const void*,u32,u32*);
int main(){u32 n=0;TlsDiag::clear();result=MAKERESULT(123,204);assert(__wrap_sslConnectionRead(&connection,buffer,8,&n)==result&&n==3);assert(!TlsDiag::lastFailure().result);
result=MAKERESULT(123,999);assert(__wrap_sslConnectionRead(&connection,buffer,8,&n)==result&&n==3);assert(TlsDiag::lastFailure().result==result);assert(!strcmp(TlsDiag::lastFailure().operation,"ssl-read"));
result=0;assert(__wrap_sslConnectionWrite(&connection,buffer,8,&n)==0&&n==4);assert(TlsDiag::lastFailure().result==MAKERESULT(123,999));TlsDiag::clear();result=MAKERESULT(123,777);assert(__wrap_sslConnectionWrite(&connection,buffer,8,&n)==result);assert(TlsDiag::lastFailure().result==result);assert(!strcmp(TlsDiag::lastFailure().operation,"ssl-write"));TlsDiag::clear();assert(!TlsDiag::lastFailure().result);}
''')
 subprocess.run(['clang++','-std=c++20','-D__SWITCH__','-I'+str(p),'-I'+str(root),str(root/'tlsdiag.cpp'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 print('PASS: SSL wrapper preserves results, arguments and byte counts; ignores WouldBlock; retains first failure; reset clears')
