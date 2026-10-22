# Client regressions

Run on macOS with Python 3, clang/clang++ and libcurl:

```sh
python3 tests/client/check.py .
python3 tests/client/reuse-check.py .
python3 tests/client/tls-wrap-check.py .
```

The tests compile the real client HTTP, sync, fingerprint, ZIP, archive and restore code. Only Switch mount/commit APIs are replaced with a temporary filesystem; SHA-256 uses CommonCrypto. They exercise failed/raced uploads, content changes, BCAT, corrupt/unsafe archives, restore bytes, no-overwrite rename recovery, archive write failures, connection reuse and SSL wrapper semantics. No console saves are accessed.

An optional second argument to `check.py` is a local save archive for sandbox restore verification. Keep real save archives private. `verification.json` is a generated local report.
