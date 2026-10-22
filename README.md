# uNX Save Sync
uNX Save Sync (aka. uNSS) is a Nintendo Switch application that allows synchronization of save data between multiple devices through a central remote server.

central remote server manages save data with internal revision IDs for each user and title.

## Fork updates and releases

See [client fixes and verification limits](docs/client-fixes.md) and [client regression tests](tests/client/README.md). GitHub Actions runs client and server tests, builds the matching app/module and uploads downloadable artifacts on every push to `main` and pull request. Pushing an `unss-v*` tag publishes a GitHub release only after all tests and the Switch build pass. Release ZIPs contain the app under `switch/uNSS/`; configure the app and install/update the bundled background service through its UI. Existing credentials, save data and boot flags are not bundled.

# Usages
## Client
![uNSS Client Screen](resources/clientscreen.jpg)
* How save data to push to remote? __JUST PRESS PUSH BUTTON__.
* How save data to pull from remote? __JUST PRESS PULL BUTTON__.

### Configuration
To use remote server synchronization, you must configure settings first. and uNSS client reads settings from `sdmc:/uNSS/config.ini`

```ini
[remote]
enabled=1
serverUrl=http://your.hostname.com:8989
; If the server sits behind basic auth, put the credentials in the URL —
; the client has no login prompt and passes this straight to libcurl, which
; sends them as an Authorization header:
;   serverUrl=https://switch:<password>@unss.example.com
; Use a long alphanumeric password — see "Server" below for why the
; punctuation that would need escaping is better avoided entirely.

; The server certificate is verified by default. If it is signed by a root
; the console does not know (a private CA), put that root in
; sdmc:/uNSS/cacert.pem — a single PEM file is enough.
; 1 turns verification off. Last resort only: without it, anyone who can
; answer the handshake receives the password, which libcurl sends
; preemptively as an Authorization header.
insecureSkipVerify=0

[account]
; Nickname of the Switch user profile to operate on.
; Must exactly match the nickname shown in the system's "My Page".
defaultAccountName=MyNickname
; 1 (default): use psel applet (profile selector) first,
;              fall back to defaultAccountName only when that fails
;              (this is always the case in applet mode, since a
;              library applet cannot launch psel).
; 0          : skip psel entirely and always resolve the account
;              from defaultAccountName.
useProfileSelector=1

[title]
; Which titles to include when pushing save data (archiving).
; "created" (default): only titles that already have save data on this console.
; "all"               : every installed title, even if it has never been saved.
archiveBy=created

; Which titles to include when pulling save data (restoring).
; "all" (default)     : every installed title, even if it has never been saved.
;                       uNSS will automatically create save data for titles
;                       that don't have any yet.
; "created"           : only titles that already have save data on this console.
restoreBy=all

; Exclude specific titles by title ID (hex, comma-separated).
; excludedTitleIds=0100000000010000,010000000000100B

; Exclude specific titles by name (separated by "||").
; excludedTitleNames=uNSS||DBI

[sync]
; 1: push automatically on launch, without pressing anything.
;    Also read by the background service (see below).
autoPushOnLaunch=0
; Minimum hours between two automatic backups — a brake, not a trigger.
; What actually decides whether anything is uploaded is whether the save
; data changed; unchanged titles are skipped either way.
; 0 (recommended): check on every launch and upload whatever changed. If
;                  nothing changed, nothing happens — for weeks, if that is
;                  how long it takes.
; >0             : additionally refuse to even look before that many hours
;                  have passed. Only useful to cap the checking itself.
autoPushIntervalHours=0
; 1: back up every user profile registered on the console.
; 0: only the one named in defaultAccountName.
allAccounts=1
```

#### `[account]` behavior matrix

| Launch context | `useProfileSelector=1` (default) | `useProfileSelector=0` |
|---|---|---|
| Full application mode (forwarder / title takeover) | Use psel applet → fall back to `defaultAccountName` | Always use `defaultAccountName` |
| Applet mode (hbmenu via album applet) | Use `defaultAccountName` (psel is unavailable to library applets) | Use `defaultAccountName` |

If `defaultAccountName` is unset (or does not match any registered user) when the client needs it, uNSS prints an explanatory message and only the Exit option is available.

### Automatic backup

With `autoPushOnLaunch=1` the client starts pushing as soon as it opens — no
menu interaction. Two rules keep that from being wasteful or unsafe:

* **Only what changed.** The newest modification time inside each save is
  compared against the last upload (`sdmc:/uNSS/saves/.syncstate`). Unchanged
  titles are skipped before they are even archived.
* **Not while playing.** A running game keeps its save file open, so a backup
  taken at that moment can be inconsistent. The automatic push waits; the
  manual *Push to Server* button is never blocked.

The timestamp for `autoPushIntervalHours` is only written after a successful
run, so a failed backup is retried on the next launch instead of being
counted as done.

## Background service

An NRO only runs while it is open — start a game and it is gone. For backups
that happen without you, uNSS ships a sysmodule that starts with the console
and stays resident.

It works, on real hardware — firmware 22.5.0 with Atmosphere 1.11.2, backing up
three accounts unattended. It is also the part of uNSS that can take your
console down if the console has no room for it, so read
[Requirement: room in the system memory pool](#requirement-room-in-the-system-memory-pool)
before installing. Everything else here is only interesting once that holds.

It wakes every few minutes and asks one question: has any save data changed
since the last upload? If not, it does nothing at all — not even opening a
socket — and goes back to sleep. That can go on for weeks. When a game ends
it checks immediately rather than waiting for the next interval, since the
moment right after someone saves and quits is the best time to copy a save.
While a game is running it never touches save data, because the game holds
those files open.

Backing up only at boot, which is what earlier versions did, sounds
reasonable and works badly: a Switch is closed, not switched off, so reboots
are weeks apart. What triggers a backup is change, not the clock.

Install it from inside the app: **Install background service**. It unpacks the
module to `atmosphere/contents/4200000000554E53/`, sets the boot2 flag and
takes effect after a **reboot**. **Remove background service** undoes it.
An already installed module is updated silently when the app carries a newer
one; only the first install is a deliberate button press, since it starts a
process at every boot.

The service reads the same `config.ini` and needs `remote.enabled=1` and
`sync.autoPushOnLaunch=1`. It has no user interface, so it cannot show a
profile selector: with `allAccounts=0` it depends entirely on
`defaultAccountName`, which is compared **case-sensitively**.

Each account only gets the saves that belong to it. The console hands back
every save on the system when asked for a list, without separating them by
user, so a save one player owns is not a failure for the other two — it is
simply not theirs, and is skipped rather than logged as an error.

It writes to `sdmc:/uNSS/sysmodule.log`, and the app can read that file back:
**Service log** follows the end of it while new lines arrive, and lets go as
soon as you scroll up. Lines mentioning a failure are red. A sysmodule has no
screen of its own, so without this the only way to find out what it had been
doing was to pull the SD card or fetch the log over FTP.

Restoring is deliberately not part of it — writing save data back should be a
decision you watch happen, in the app.

### Requirement: room in the system memory pool

**This is where the background service can take your console down.** Read it
before installing.

Sysmodules do not get memory of their own. They all draw from one system pool
that the console's own processes are already living in, and it got tighter
with firmware 20.0.0. Measured on 22.5.0, that pool is **232 MB in total —
with about 210 MB of it already spoken for before any homebrew loads.** The
twenty-odd megabytes left over are what `sys-ftpd`, `sys-patch`, overlay
loaders like `nx-ovlloader`, Tesla, emuiibo and this module have to share.

A sysmodule's heap is a static array, so its full size leaves that pool the
moment the module loads — whether it ever uses a byte of it or not.

When it runs out, the kernel refuses the next allocation and whichever system
process asked for it dies — `hid` (no controller input) or `am` (the console
stops). The error is `2001-0132`, kernel `LimitReached`, and it names *the
victim*, not the cause. So the console blames a Nintendo process while the
module that exhausted the pool keeps running, looking innocent.

**So: run as few other sysmodules as you can.** On a console loaded with
resident modules, uNSS may not fit — and the way you find out is a boot loop,
not an error message.

The pool is one of two limits that behave like this. The other one is service
manager sessions, and it shows up as homebrew refusing to launch at all; see
[A second limit](#a-second-limit-service-manager-sessions) below.

#### How much has to be free

**There is no number here, and the measurements are the reason.** This section
exists to stop you trusting one.

Ten module starts were logged on one console (22.5.0). Free system pool at
start ranged from 13.5 MB to 21.9 MB. Every single start shows the same
pattern: about **9 MB disappears within five seconds** of the module coming
up, and in most runs it stays gone. Then:

| free at start | low point | outcome |
|---|---|---|
| 15076 KB | 4780 KB | log stops mid-startup — the console died |
| 21100 KB | 11568 KB | survived |
| 21868 KB | 12824 KB | survived |
| 20144 KB | 10612 KB | survived |
| 13496 KB | **3448–3960 KB** | survived — **five times** |

Read the first and last rows again. The run that died had *more* memory left
at its low point than five runs that lived. Free memory does not separate
them, so no threshold can honestly be drawn from this data — not "20 MB at
start", not "keep 5 MB free". Anyone quoting such a number, including an
earlier version of this file, is interpolating.

What the crashes actually had in common was fixed in code, not in free space:
a 6 MiB inner heap, then a 2 MiB one, and a 144 KB structure on a 16 KB stack.
Since those went, the module has survived every logged round — including the
five at 3.4 MB.

For context, those figures were measured with **nine sysmodules starting at
boot** — sys-ftpd, sys-patch, SaltyNX, MissionControl, sys-clk, NxThemes, two
more and uNSS itself. Counting folders under `atmosphere/contents/` overstates
it: several are LayeredFS entries with no `flags/` directory, and one was
disabled by renaming its flag. Only a folder containing `flags/boot2.flag`
costs anything.

So the advice stays qualitative, because that is all the evidence supports:
**every resident sysmodule you remove is headroom you get back, and the pool
is the thing that kills consoles.** Removing three overlay loaders moved this
console from 15 MB free to 21 MB. Note how small the modules are that buy that
back — emuiibo 262 KB, MissionControl 187 KB, sys-clk 174 KB, sys-ftpd 173 KB.
What costs the pool is rarely the file size, because a loader reserves room
for the largest thing it might load.

Worth checking while you are in there: a module can start at boot, take its
share of the pool and do nothing useful. On this console sys-ftpd had a live
`boot2.flag` but served no FTP — it is built for firmware 19.0.0 and this is
22.5.0 — and left no crash report to hint at it. Anything that has not been
verified to work since the last firmware jump is worth a look.

You do not have to guess on your own console: the module writes what it found
on every start, and **Service log** in the app shows it.

```
pool system at start: <used> of <total> KB used, <free> KB free
pools +0s  (KB free): app=... applet=... sys=<free> sys-unsafe=...
pools +5s  (KB free): ...                 sys=<the dip>
pools +10s (KB free): ...                 sys=...
heap after round: <in use> KB in use, <reached> KB reached, 1024 KB total
```

The `sys=` column is the pool everything competes for. Watch how it moves
rather than what it reads once: a single value at startup misses the drop
entirely, which is why the module samples every five seconds through the whole
grace period.

Use it to compare your console against itself — before and after removing a
module — not against the numbers above. Those came from one console on one
firmware, and they demonstrably fail to predict a crash.

The sampling earns its keep in a different way. The crash it was built to
catch happens inside those thirty seconds, where nobody can see it: the
console simply dies, screen and all. The log closes the file after every
line, so whatever was written last survives. That is how the fatal run above
is identifiable at all — not by an error message, but by the log simply
stopping after `+10s`.

The `heap` line is uNSS itself, so you can see how much of its 1 MiB is real
(measured: about 190–250 KB).

If it does boot-loop, the console is not bricked: delete
`atmosphere/contents/4200000000554E53/flags/boot2.flag` from the SD card on a
PC and it comes up clean.

`INNER_HEAP_SIZE` is **1 MiB**, and that number was earned the hard way:

| size | outcome |
|---|---|
| 6 MiB | starved `hid` — boot loop |
| 2 MiB | killed `am` |
| 1 MiB | carries a full round end to end ✅ |
| 256 KB | too tight — see below |

Do not raise it, and do not lower it either. 256 KB looked justified: a full
round across three accounts had peaked at 189 KB. At 256 KB the *same* round
peaked at 245 KB instead — a high water mark is not a property of the program
alone. A tight heap fragments and wastes what a roomy one reuses, so the
measurement was never a lower bound. Everything downstream failed at once:
compression (`ret=-3`), title lookup (the 144 KB control record no longer fit,
so every game logged as "Unknown"), and the upload.

The same pool is why nothing heavy is opened during boot. Network services
wait for a 30 second grace period *and* for there to be something to upload;
`ns` and `account` open after the grace period, because the change check needs
them. When nothing changed, no socket is ever opened — which is the normal
case, and costs the pool nothing.

#### A second limit: service manager sessions

The pool is not the only thing you can run out of, and the other limit fails in
a way that points nowhere near the cause.

On the console above, installing one more resident sysmodule — `ftpsrv` as a
sysmodule, alongside uNSS — made **every homebrew launch** take the console
down. Not eventually: opening Sphaira right after a reboot was enough, every
time. The screen showed

```
Atmosphere panic occurred!

Title ID: 0100000000000034
Error:    std::abort (0xFFE)

Report saved to atmosphere/fatal_errors/report_XXXXXXXX.bin
```

Two things about that screen are traps.

`0100000000000034` is `fatal` — **Atmosphère's own crash reporter**. It is not
the program that failed. A process could not start, libnx called `fatalThrow`,
and `fatal` went to display it; to do that it needs `lbl`, the backlight
service, and it could not get that either. So it aborted, and the bare panic
screen is what is left when even the error handler dies. The original failure
is never named.

And the report is in `atmosphere/fatal_errors/`, **not** `fatal_reports/` or
`crash_reports/` — those two stay empty, because `fatalThrow` is a deliberate
abort, not a CPU exception. Looking in the obvious place suggests nothing
happened at all.

The `.bin` is small and readable without tools:

```
00000000: 4146 4532 fe0f 0000 3400 0000 0000 0001  AFE2....4.......
00000010: 1506 0000 ...                            ....
...
00000360: 5346 434f 0000 0000 1506 0000 0000 0000  SFCO............
00000370: 6c62 6c00                                lbl.
```

`AFE2` is the magic, `0x0FFE` the abort, then the program ID. The `SFCO` block
near the end is a captured IPC reply: result `0x615` on service `lbl`. Split
that the way Horizon does — module `0x615 & 0x1FF` = 21, description
`0x615 >> 9` = 3 — and it reads **`2021-0003`, `sm::ResultOutOfSessions`**.
The service manager was out of sessions. It has 88 of them, 87 for processes.

Removing the extra sysmodule fixed it: reboot, launch homebrew, no panic.

**What this does and does not establish.** It is one A/B on one console with one
module, so it does not show that `ftpsrv` is special — only that *one resident
sysmodule too many* was enough, and that the failure lands nowhere near
whatever pushed it over. Nor does it identify what holds the 87 sessions;
that cannot be read from outside. Four candidates inside uNSS were checked
against the source and cleared: network services are opened once behind a
guard, the save-data info reader is closed, save mounts are released by an RAII
guard on every error path, and nothing re-initialises per round.

The practical rule is the same as for the pool, for a different reason:
**every resident sysmodule you remove is headroom.** If homebrew stopped
launching after you added one, take it out before looking anywhere else.

### If it ever takes the console down again

The module cannot be trusted to be correct — it has taken HID with it twice.
So it does not rely on being correct.

Before doing anything risky it writes `sdmc:/uNSS/.running`, recording how
many crash reports existed at that moment and which phase it was in, and
deletes the file once it has survived the round. Finding that file at the
next start means the previous round did not finish, and comparing the counts
says whether anything actually died:

* `atmosphere/crash_reports/` — a normal program died. The filename carries
  the program id, so a report naming `4200000000554e53` is **us**, beyond
  doubt. The `2168-0002` stack overflow was here.
* `atmosphere/fatal_reports/` — a *system* process died, and the console
  went down with it. This one is not proof of guilt: any process crashing
  inside our window lands here.

Both directories have to be read; the two real incidents landed in different
ones. Filenames are lowercase, and a case-sensitive search for an uppercase
program id finds nothing — which reads like "no reports at all".

What follows depends on **when** it happened, because the two cases are not
equally dangerous:

| when | what died | response |
|---|---|---|
| during startup | anything | **disables itself** — a retry here is a boot loop |
| during a backup | only us | pause 5 min, then 20, then 80, capped at 2 h — then retry on its own |
| during a backup | the console (fatal) | pause the full 2 h at once; **disables itself** if it happens twice |
| — | nothing (power cut) | carries on |

Disabling means renaming its own `boot2.flag` to `boot2.flag.crashed`, so the
next boot comes up without it. The app then offers **Re-enable background
service**.

Pausing rather than switching off is the deliberate part. Nobody is watching a
console for notifications, so a module that turned itself off would simply stay
off forever — and the first strike is often not even ours, since someone else's
game crashing during our few minutes counts the same. But a fatal report means
the whole console stopped, which cannot be treated like one failed backup:
retrying every five minutes turns it into a device that dies every five
minutes. That happened (`am`, twice, 273 seconds apart), which is why fatals
back off all the way immediately.

Counting reports rather than comparing timestamps is deliberate too: right
after boot the clock may not be set yet, so times are not trustworthy. A count
only ever grows.

## Server
### Prerequisite
Running server via Python interpreter requires some dependencies. Install dependencies first.
```bash
pip install -r requirements.txt
```

### Linux / macOS
Foreground mode

```bash
./run-linux.sh
```

or

```bash
python main.py --host 0.0.0.0 --port 8989
```

Background mode

```bash
setsid nohup ./run-linux.sh > server.log 2>&1 < /dev/null &
```

`nohup` alone is not enough: it only shields against `SIGHUP`. Closing the
terminal or pressing Ctrl-C sends `SIGINT` to the whole foreground process
group, and uvicorn shuts down cleanly on that — the log then shows a tidy
shutdown rather than a crash, which is easy to misread. `setsid` puts the
server in its own session, out of reach of both.

Run it from the `server/` directory: `metadata.sqlite` and `savedata/` are
resolved relative to the working directory.

### Tests

```bash
pip install -r requirements-dev.txt
pytest
```

Run them from the `server/` directory. Each test gets a fresh server in a
temporary directory, so they neither touch nor need your real database.

They cover the two places where a mistake is expensive and silent: the wire
format the Switch client parses by hand, and the revision transaction
(`P` → `C`/`D`) that decides which backup counts as current. A revision that
never finished must not hide the last good one, and an upload must not be
able to overwrite a completed revision.

The `tests.sh` script next to them is something else — a handful of `curl`
invocations for poking at a running server by hand. It checks nothing.

### Windows
Just used prebuilt binary by PyInstaller

### Docker

```bash
docker compose -f docker-compose.yaml up -d
```

`compose.traefik.yml` is an alternative for setups that already run Traefik:
the container publishes no port of its own and is reached inside the docker
network, with TLS terminated by the proxy.

Access control there is basicAuth, not a login portal — the client does not
follow redirects, so anything redirect-based (authelia, OIDC) can never
complete. libcurl does send credentials taken straight from the URL, so
`serverUrl=https://user:password@host` works without any client change.

You supply two values, both in plain text:

```
UNSS_USERNAME   defaults to `switch`
UNSS_PASSWORD
```

The stack hashes the password itself, with bcrypt at cost 12. Doing it there
rather than by hand keeps it to **one** value: the same password has to reach
the client too, and a hash cannot be turned back into it — maintaining both
meant keeping them in sync by hand. To hash elsewhere anyway, set
`UNSS_BASICAUTH_USERS` to a full htpasswd line and the password is ignored.

The explicit cost matters: `htpasswd` defaults to 5, far too low, and Traefik's
basicAuth supports only MD5, SHA1 and bcrypt (no argon2), so cost and password
length carry the security.

**Which characters the password may contain is not a free choice.** The client
takes its credentials from the URL and passes that string to libcurl
unescaped, so `/`, `?`, `#`, `@`, `%`, `[` and `]` change how the URL is split
and break authentication in ways the error message does not point at. The INI
parser also strips `'` and `"` from both ends of a value. Use a long
alphanumeric password — length carries the entropy, not the variety of
characters:

```bash
LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 32; echo
```

Getting the resulting hash to Traefik is the one genuinely awkward part of this
setup, because **a container label cannot be filled from a secret store.**

Traefik reads labels from the docker daemon — container metadata, fixed when
the container is created. It never looks inside the container, so an
environment variable a secret store populates at startup is invisible to it,
and compose has already written the label by then. The label simply stays
empty. That is not a harmless default either: Traefik discards a router whose
middleware is invalid, so the service answers **404** rather than asking for
credentials, which is easy to misread as a routing problem.

Two ways around it. They differ in where the hash is *maintained* — not in
whether it lands in a file on disk, which it does either way.

**From a secret store** (what `compose.traefik.yml` does). A small helper
container reads the credentials, hashes the password and writes Traefik's
middleware into its dynamic configuration; Traefik picks the file up on its
own. The store fills a container environment, which it can do, and the
container writes a file, which Traefik reads — that bridges the gap. Rotating
the password means changing it in one place. With OpenMediaVault's compose
plugin, locket can fetch it from OpenBao:

```yaml
locket:
  provider:
    type: locket
    options:
      provider: bao
      bao-url: http://127.0.0.1:8200
      bao-role-id: <role id>
      bao-secret-id: file:/path/to/role-secret-id
      raw: true
      env:
        - UNSS_USERNAME={{bao://secret/unss/UNSS_USERNAME}}
        - UNSS_PASSWORD={{bao://secret/unss/UNSS_PASSWORD}}
```

Note that bcrypt salts randomly, so the generated line differs on every start
even though the password does not. Traefik re-reads the file and carries on —
but only if its file provider is actually watching, which is not the default:

```yaml
providers:
  file:
    directory: /srv/traefik/dynamic
    watch: true
```

Without `watch: true` Traefik reads the directory once at its own startup, so a
freshly written middleware is never picked up and the router referencing
`unss-auth@file` is dropped — the 404 again, not a 401.

The router then references the middleware as `unss-auth@file`, not
`unss-auth` — it comes from the file provider, and without the suffix Traefik
looks for one the docker provider never defined.

**From an `.env` file**, feeding the same writer. Fewer moving parts, but the
`.env` becomes the source of truth. Under OpenMediaVault the stack's
*Environment* field is that file.

An `.env` on its own does **not** reach a container — it only fills `${...}`
in the compose file, and the credentials are deliberately not interpolated
there. So this route needs one more line on `unss-auth-writer`:

```yaml
env_file:
  - .env
```

```
UNSS_PASSWORD=<32 alphanumeric characters>
```

A plain password needs no escaping. A ready-made hash still does, since the
doubled `$` is collapsed on the way in:

```
UNSS_BASICAUTH_USERS=switch:$$2y$$12$$....
```

Note that `environment:` has none of these problems — it is read at runtime,
so a secret store works there without any of this. Labels are the exception.

The client needs the **plaintext** password, since that is what it sends. The
user name in the URL is whatever `UNSS_USERNAME` was set to — `switch` unless
you changed it:

```ini
serverUrl=https://switch:<password>@unss.example.com
```

This is not the browser form of the URL — a browser prompts for credentials
in a dialog, and nothing is ever typed into the address bar. The client has
no dialog to prompt with, so it takes them from the URL; libcurl strips the
`user:pass@` part off and sends it as an `Authorization` header, exactly as a
browser would. The password never appears in the request path.

It does, however, sit in plaintext in `config.ini` on the SD card, and there
is no way around that: Switch homebrew has no key store, so a client
certificate would be equally exposed. This is bearable because of what it
guards — anyone holding the SD card already has the save data the password
protects. What it really keeps out is the open internet. Use a random
password used nowhere else, and if the card is ever lost, change
`UNSS_PASSWORD` and recreate the stack; the old password is worthless from
that moment.

Basic auth transmits that password on every request, protected only by TLS.
Over plain HTTP it is trivially readable — so use HTTPS, or no authentication
at all on a trusted network, but never basic auth over HTTP.

The client verifies the server certificate by default, which is what makes
that password safe to send. Let's Encrypt needs nothing extra — the console
has trusted ISRG Root X1 since firmware 10.1.0. For a private CA, drop the
root into `sdmc:/uNSS/cacert.pem`; this curl uses the libnx SSL backend, which
passes `CAINFO` to `sslContextImportServerPki`, so one file is enough to make
the console trust a root the firmware never shipped.

`remote.insecureSkipVerify=1` turns the check off. It is the last resort, not
the first thing to try, because its failure mode is misleading: a rejected
certificate looks like "cannot connect" rather than "certificate refused", so
the temptation is to disable verification and move on. With it off, anything
that can answer the handshake — hostile Wi-Fi, a spoofed DNS answer —
receives the plaintext password, which libcurl sends preemptively. The bcrypt
cost hardens the hash at rest; it does nothing for that path.

Images are published to `ghcr.io/<owner>/unss-server` by
`.github/workflows/publish-server-image.yml` on pushes to `main` and on `v*`
tags, for amd64 and arm64.
