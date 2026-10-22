from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import threading, subprocess, tempfile,sys
root=Path(sys.argv[1]).resolve()/'client/source'
received=[]
class Handler(BaseHTTPRequestHandler):
 protocol_version='HTTP/1.1'
 def log_message(self,*a):pass
 def do_GET(self):self.respond(b'')
 def do_POST(self):
  body=b''
  if self.headers.get('Transfer-Encoding')=='chunked':
   while True:
    n=int(self.rfile.readline().strip(),16)
    if not n:self.rfile.readline();break
    body+=self.rfile.read(n);self.rfile.read(2)
  else:body=self.rfile.read(int(self.headers.get('Content-Length','0')))
  self.respond(body)
 def respond(self,body):
  received.append((self.client_address[1],self.command,body,self.headers.get('X-Test')))
  self.send_response(200);self.send_header('Content-Length','2');self.end_headers();self.wfile.write(b'OK')
s=ThreadingHTTPServer(('127.0.0.1',0),Handler);s.daemon_threads=True;threading.Thread(target=s.serve_forever,daemon=True).start()
source='''#include "http.hpp"
#include <cstring>
int main(int argc,char**argv){HTTPClient shared;for(int i=0;i<3;i++){HTTPClient fresh;HTTPClient& c=argc>2?fresh:shared;c.reset().setUrl(argv[1]).setMethod(i==2?"GET":"POST");bool sent=false;std::string response;if(i==0)c.setHeader("X-Test","first");if(i==1)c.setSendCallback([&](void*p,size_t n,size_t&a){a=sent?0:3;if(a>n)return false;if(a)memcpy(p,"abc",3);sent=true;return true;});else c.setReceiveCallback([&](const void*p,size_t n,size_t&a){response.append((const char*)p,n);a=n;return true;});if(c.perform()!=200)return 1;if(i!=1 && response!="OK")return 2;}return 0;}'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'main.cpp').write_text(source)
 subprocess.run(['clang++','-std=c++17','-I'+str(root),str(p/'main.cpp'),str(root/'http.cpp'),str(root/'tlsdiag.cpp'),'-lcurl','-o',str(p/'probe')],check=True)
 url=f'http://127.0.0.1:{s.server_port}/'
 for args,connections in [(['fresh'],3),([],1)]:
  received.clear();subprocess.run([str(p/'probe'),url]+args,check=True,timeout=10)
  assert len(set(r[0] for r in received))==connections,received
  assert [(r[1],r[2],r[3]) for r in received]==[('POST',b'','first'),('POST',b'abc',None),('GET',b'',None)],received
s.shutdown();print('PASS: fresh clients open 3 connections; reused client opens 1; methods, callbacks, payloads and headers reset correctly')
