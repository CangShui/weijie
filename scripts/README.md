# Scripts

- `build.ps1`: native build, test, package and clean pipeline.
- `deploy-installed.ps1`: stops the installed instance, replaces the executable, arranges existing bundled pictures under `images/default` on first upgrade, starts it again. Preserves user edits and deletions, including deletion of the entire default group.
- `images-layout.ps1`: initializes a fresh image library or relocates surviving old default pictures. Grouped installs and packages are left untouched on subsequent updates.
- `test-images-layout.ps1`: isolated tests for image relocation and preserving user deletions, run by the UI test suite.
- `run.ps1`: launches the latest Release application.
- `cleanup-old-build.ps1`: one-time migration helper for the old `cpp/build` layout.

Use the root `build.bat` / `run.bat` wrappers in normal work.
