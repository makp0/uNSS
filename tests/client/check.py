from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import tempfile,subprocess,threading,zipfile,struct,warnings,json,sys,hashlib
ROOT=Path(sys.argv[1]).resolve()/'client/source'
HERE=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='unss-regression-') as td:
 base=Path(td);fixture=base/'fixtures';fixture.mkdir();blobs={};requests=[]
 def archive(name,entries):
  with warnings.catch_warnings():
   warnings.simplefilter('ignore')
   with zipfile.ZipFile(fixture/name,'w') as z:
    for path,data in entries:z.writestr(path,data)
 archive('corrupt.zip',[('saves/state',b'original')]);p=fixture/'corrupt.zip';b=bytearray(p.read_bytes());off=30+struct.unpack_from('<H',b,26)[0]+struct.unpack_from('<H',b,28)[0];b[off]^=1;p.write_bytes(b)
 archive('traversal.zip',[('saves/../escape',b'evil')]);archive('duplicate.zip',[('saves/state',b'a'),('saves/state',b'b')]);archive('absolute.zip',[('/saves/state',b'x')]);(fixture/'invalid.zip').write_bytes(b'not a zip')
 class Handler(BaseHTTPRequestHandler):
  protocol_version='HTTP/1.1'
  def log_message(self,*args):pass
  def respond(self,status,data):
   self.send_response(status);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
  def do_POST(self):
   assert self.headers.get('Transfer-Encoding') is None,'known-size uploads must avoid chunking'
   data=self.rfile.read(int(self.headers.get('Content-Length','0')));requests.append((self.path,len(data)))
   if self.path.endswith('/revisions'):return self.respond(200,b'fixture-revision')
   if self.path.startswith('/fail/'):return self.respond(503,b'failed')
   blobs[self.path]=data
   if self.path.startswith('/race/'):(base/'profile/state').write_bytes(b'after-upload')
   self.respond(200,b'OK')
  def do_GET(self):
   if self.path.endswith('/revisions'):return self.respond(200,b'fixture-revision')
   if self.path.startswith('/corrupt/'):return self.respond(200,b'corrupt zip')
   self.respond(200,blobs.get(self.path.removesuffix('/data'),b''))
 server=ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True;threading.Thread(target=server.serve_forever,daemon=True).start()
 # Build the real generic traversal functions; only Switch mount/commit APIs are faked.
 src=(ROOT/'fileio.cpp').read_text();(base/'fileio-generic.cpp').write_text('int testRename(const char*,const char*);\n'+src[:src.index('int createSaveData(')].replace('rename(', 'testRename('))
 subprocess.run(['clang','-O1','-I'+str(ROOT),'-c',str(ROOT/'miniz.c'),'-o',str(base/'miniz.o')],check=True)
 sources=[HERE/'driver.cpp',base/'fileio-generic.cpp']+[ROOT/n for n in ['fingerprint.cpp','zipio.cpp','savedata.cpp','sync.cpp','remote.cpp','http.cpp','tlsdiag.cpp']]
 cmd=['clang++','-std=c++20','-O1','-Wno-deprecated-declarations','-I'+str(HERE/'shim'),'-I'+str(ROOT)]+list(map(str,sources))+[str(base/'miniz.o'),'-lcurl','-o',str(base/'check')]
 subprocess.run(cmd,check=True)
 subprocess.run([str(base/'check'),str(fixture),f'http://127.0.0.1:{server.server_port}'],cwd=base,check=True,timeout=40)
 if len(sys.argv)>2:
  archive=Path(sys.argv[2]).resolve();restored=base/'real-restore';restored.mkdir()
  subprocess.run([str(base/'check'),str(archive)],cwd=restored,check=True,timeout=60)
  expected={}
  with zipfile.ZipFile(archive) as z:
   for info in z.infolist():
    if not info.is_dir():expected[info.filename]=hashlib.sha256(z.read(info)).hexdigest()
  actual={}
  for name,folder in [('saves','profile'),('bcat','bcat-data')]:
   for file in (restored/folder).rglob('*'):
    if file.is_file():actual[name+'/'+file.relative_to(restored/folder).as_posix()]=hashlib.sha256(file.read_bytes()).hexdigest()
  assert actual==expected,(set(actual),set(expected))
  print('PASS: downloaded server archive restored by actual client restore code into sandbox; all',len(actual),'files match SHA-256')
 server.shutdown();print('HTTP request count:',len(requests),'successful uploads:',len(blobs))
 (HERE/'verification.json').write_text(json.dumps({'status':'passed','requests':len(requests),'successful_uploads':len(blobs),'filesystem':'temporary sandbox; no console game save modified','tested':'actual native-build client HTTP, sync, hashing, ZIP and restore implementations'},indent=2)+'\n')
