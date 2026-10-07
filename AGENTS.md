# Tarman working and release flows

## Scope and defaults

This repository is Tarman, a Windows task manager and resource monitor written in
C11 on raylib, built with CMake + Ninja from the MSYS2 MinGW-w64 toolchain
(`build.ps1`). It is not an Atlas project and does not use gobake. Forge is the
sibling `../Forge` project. clockt and Descry are reference implementations of
these flows, not Tarman's release target.

Preserve existing changes when working in a dirty checkout.

## Code layout rules

- Win32 and raylib must never be included in the same translation unit: both
  define `Rectangle`, `DrawText`, `CloseWindow`, `LoadImage` and more.
  `src/sys.h` is the plain-C boundary. Files that call Win32 (`sys_*.c`,
  `hoswl.c`) never include `raylib.h`; UI files never include `windows.h`.
- The UI thread holds `sys_lock()` for the whole frame. Pages must not keep a
  `Proc*` across frames; selections are `(pid, create_time)` pairs.
- All rings are indexed by the global sample number (`k % HIST_LEN`); keep new
  histories on the same clock.
- Colours come from the `Theme` struct (`T.*`), never literals in page code.
- `--screenshot` runs ignore all input and do not connect to Hisashi.

## Commit messages

Use a normal commit title and body only. Do not add `Co-Authored-By` trailers or
AI/assistant attribution (including Claude or Codex) to commits or release notes.
For multiline messages or messages containing quotes, write a temporary UTF-8
message file and use `git commit -F <file>`. Check the exit code and verify the
resulting commit before tagging; PowerShell 5.1 can split inline quoted messages.

## Version sources of truth

`src/version.h` (`TARMAN_VERSION`) is the single source of truth. The app prints
it for `--version` and the About box, and CMake parses it into the exe's
VERSIONINFO. `version.ps1` keeps the inputs that cannot read the header in step:
the `forge.toml` `[app] version` (wizard strings and the registry value read it
back as `${app.version}`) and the installer filename in `README.md`.

```powershell
.\version.ps1                  # report every location + consistency
.\version.ps1 -Bump patch      # 0.1.0 -> 0.1.1 everywhere, then verify
.\version.ps1 -Set 0.2.0       # set an exact version, then verify
.\version.ps1 -RequireBuild    # also verify build\tarman.exe
```

## RELEASE workflow

Only an explicit request to **RELEASE** triggers the complete publishing flow.
Ordinary fixes, builds, and installer requests do not imply a version bump,
commit, push, tag, or GitHub release. When RELEASE is requested, perform these
steps in order and stop/report any failure before proceeding:

1. Run `./version.ps1 -Bump patch` by default, or `-Set x.y.z` for a requested
   version (the first public release is 0.1.0 as-is). Run `./version.ps1` to verify.
2. Run `./installer.ps1`. It builds (stopping any Tarman running from this
   repo's `build\`), verifies the version in the sources and the built exe,
   ensures a GUI-subsystem `../Forge/build/forge.exe` (needs `uninstall.exe`
   from `gobake build` in Forge), and produces
   `dist/installer/Tarman-Setup-<version>.exe`. Surface that exact path. An
   installed copy stays old until Setup is run; keep the wizard's
   "Run Tarman after install" option enabled.
3. Review and commit the intended changes without attribution, using a message
   file. Verify the commit landed and record its hash.
4. Push to `origin main` (`https://github.com/fezcode/Tarman`). Inspect
   `git remote -v` and the branch first. Never borrow another project's remote
   or force-push.
5. Create the matching `vX.Y.Z` tag on the verified commit, push it, and create
   a GitHub release with `gh release create vX.Y.Z`, attaching only
   `dist/installer/Tarman-Setup-X.Y.Z.exe`. Title `Tarman vX.Y.Z`; notes start
   with `## Tarman vX.Y.Z` followed by `### <feature>` sections and bullets (no
   emoji). Supply notes through `--notes-file`. Verify the published asset.

## Build and installer maintenance

- Keep `forge.toml` on the Mica wizard theme with Tarman's icon and the stable
  Forge ID `com.fezcode.tarman`. Keep the six wizard steps in order: welcome,
  license, folder, shortcuts, install, finish.
- The payload is `build/tarman.exe`, `build/glfw3.dll` (MSYS2's static raylib
  is compiled against the glfw DLL) and `build/LICENSE.txt`. The license is
  shipped from the build copy because Forge bundles the license step's own file
  as display-only and skips a `[[files]]` entry with the same source path.
- If `cmake/CopyRuntimeDeps.cmake` starts bundling more DLLs, add them to
  `forge.toml` and to the payload check in `installer.ps1`.
- Maintain HKCU `Software\Fezcode\Tarman` InstallDir and Version values.
- Preserve user data under `%APPDATA%/Tarman` (settings.ini, logs\) unless the
  user chooses the installer's option to remove settings/data.
- Fail on inconsistent versions, failed builds, missing payload, or non-GUI
  Setup executables. Never report an old installer as a new success.
- Check GUI Forge process exit codes with `Start-Process -Wait -PassThru`.
- `dist/installer` keeps only the current installer; `installer.ps1` prunes
  superseded ones (each is already a GitHub release) unless
  `-KeepOldInstallers` is passed.
- There is no CI. Tarman is built and released from this machine.

## Airlift

`properties.piml` holds `(airlift) true`, which invites Airlift's catalog sync
to list Tarman from `forge.toml`. Airlift resolves the Windows asset as
`Tarman-Setup-<version>.exe` from the public `fezcode/Tarman` releases, so keep
that name. A new app also needs an `editorial` entry in Airlift's
`scripts/sync-catalog.mjs`; that change belongs to the Airlift repository.
