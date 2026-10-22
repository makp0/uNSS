"""Validate matching client/module outputs and create credential-free release assets."""
from pathlib import Path
import hashlib
import re
import shutil
import zipfile

root = Path(__file__).resolve().parent.parent
nro = (root / 'client/client.nro').read_bytes()
nsp = (root / 'client-sysmodule/client-sysmodule.nsp').read_bytes()
assert nsp == (root / 'client/romfs/exefs.nsp').read_bytes(), 'ROMFS module differs'
assert nro.count(nsp) == 1, 'NRO must embed exactly one matching module'
version = re.search(r'BUNDLED_VERSION = (\d+)', (root / 'client/source/sysmodule.hpp').read_text()).group(1)
out = root / 'dist'
out.mkdir(exist_ok=True)
(out / 'uNSS.nro').write_bytes(nro)
(out / 'client-sysmodule.nsp').write_bytes(nsp)
(out / 'uNSS.version').write_text(version)
shutil.copy2(root / 'docs/client-fixes.md', out / 'NOTES.md')
shutil.copy2(root / 'LICENSE', out / 'LICENSE')
with zipfile.ZipFile(out / 'uNSS-switch.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('switch/uNSS/uNSS.nro', nro)
    archive.write(out / 'NOTES.md', 'NOTES.md')
    archive.write(out / 'LICENSE', 'LICENSE')
files = sorted(p for p in out.iterdir() if p.is_file() and p.name != 'SHA256SUMS')
(out / 'SHA256SUMS').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in files))
print(f'Packaged matching client and module {version}; SHA256SUMS written')
