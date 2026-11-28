# Repository rules

## Version and release policy

- Version 1.0.0 is published; the initial-upload freeze is over.
- Use Semantic Versioning: patch for compatible fixes, minor for compatible features, major for breaking changes. Do not bump versions for every edit.
- Update `CHANGELOG.md` and run release checks before a release. Tags must match the version in `CMakeLists.txt`.
- Never push, tag, or publish without the maintainer's request.

## Development

- Read `docs/DEVELOPMENT.md` and preserve `docs/ARCHITECTURE.md` invariants.
- Keep the native x86 app, installed/portable storage rules, startup ownership checks, and conflict-safe recovery intact.
- Add focused regression coverage for behavior changes. Run `build.ps1` and `test.ps1`; run installer and release checks when packaging changes.
- Do not commit build output, release binaries, personal configuration, logs, dumps, temporary files, or local review notes.
- Keep the first-release documentation focused on current behavior, not private development history.
