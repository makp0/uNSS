# Client reliability fixes

This fork includes changes tested with the foreground client and the background service:

- Fix empty POST bodies reporting imaginary bytes and use known archive lengths, reusable HTTP/TLS connections and five-minute transfer limits (30-second connection setup).
- Show foreground transfer progress and log completed transfer speed.
- Isolate cache and sync state by account UID. Detect Account/BCAT content and path changes with SHA-256, including deletions, renames and empty folders. The first run establishes a new fingerprint baseline and may upload existing titles again.
- Mark only successfully uploaded snapshots as synced; failed uploads and edits during upload remain pending. Background rounds stop after their first upload failure and retry later.
- Check archive creation/finalization and validate CRCs, paths and duplicates before replacing cached archives or restoring. Preserve previous caches on replacement failure, including Horizon's no-overwrite rename behavior. Propagate restore and extraction failures.
- Record curl errors, request stage, HTTP status, errno and original Switch SSL read/write Results. The linker wrappers preserve SSL behavior and ignore expected nonblocking retry results. TLS verification is unchanged.

A changed save uploaded automatically after closing the game on a real Switch; the resulting server revision was downloaded through verified TLS and its archive passed CRC checks. Native regression tests cover sandbox restore, but actual console restores have not been tested. Restore is not atomic across Account and BCAT saves; retain independent backups.

An intermittent curl 56 receive failure recovered on retry. Its underlying SSL cause remains unconfirmed; diagnostics are included to capture a recurrence. A reported freeze when using an overlay during a backup also remains unresolved. These changes are not a claim to fix either issue.

Build the app and background module together with `build-all.sh`; the NRO embeds the matching module. Current bundled module version is 20; the successful hardware upload used module 19. The publication review additionally fixed `ZipReader::extractAll` returning before all entries were extracted; its regression test covers full extraction and extraction failures. This last correction has native coverage and is not yet installed on the tested console.

See [client regression instructions](../tests/client/README.md).

CI uses a pinned official devkitPro image with preinstalled Switch dependencies; it checks packages offline rather than fetching package databases during a build. Push an `unss-v*` tag to run tests/build and publish release assets after every job succeeds.
