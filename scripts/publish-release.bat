@echo off
setlocal enableDelayedExpansion

rem ---------------------------------------------------------------------------
rem Builds Umbra and Umbra Host and publishes both to one GitHub release.
rem
rem Umbra updates itself from GitHub Releases: AutoUpdateChecker reads
rem /releases/latest and looks for an installer asset whose name contains the
rem running machine's architecture. Umbra Host does the same for its own installer
rem (host\src\self_update.cpp), and so does Umbra's Update button for other PCs.
rem
rem app\version.txt is the source of truth for both. The tag is v<version>, and the
rem assets are UmbraSetup-<arch>-<version>.exe (Umbra, with this same host bundled)
rem and UmbraHostSetup-<arch>-<version>.exe (the host on its own).
rem
rem Usage:
rem   scripts\publish-release.bat [options]
rem
rem   bump-patch      bump the third version component before building
rem   bump-minor      bump the second, resetting patch
rem   bump-major      bump the first, resetting the rest
rem   prerelease      publish as a prerelease, so it is NOT offered as an update
rem   dry-run         build and stage the assets without touching GitHub
rem   no-host         build a client-only installer, and no host
rem
rem Requires the GitHub CLI (gh), authenticated, Visual Studio for Umbra and MSYS2
rem for the host (host\scripts\umbra-build.bat lists its packages). The first run
rem fetches the host's submodules, which takes a while. Builds are unsigned, so Windows
rem SmartScreen will warn on first run; see SIGNTOOL_PARAMS in build-arch.bat for
rem where signing plugs in.
rem ---------------------------------------------------------------------------

rem Resolve our own location before parsing arguments: the shift below moves %0 as
rem well as the arguments, so %~dp0 stops pointing at this script afterwards.
set SOURCE_ROOT=%~dp0..
pushd "%SOURCE_ROOT%"
set SOURCE_ROOT=%cd%
popd

set BUMP=
set PRERELEASE=0
set DRYRUN=0
set NOHOST=0

:parse
if "%~1"=="" goto parsed
if /I "%~1"=="bump-patch"  set BUMP=patch
if /I "%~1"=="bump-minor"  set BUMP=minor
if /I "%~1"=="bump-major"  set BUMP=major
if /I "%~1"=="prerelease"  set PRERELEASE=1
if /I "%~1"=="dry-run"     set DRYRUN=1
if /I "%~1"=="no-host"     set NOHOST=1
shift
goto parse
:parsed

rem gh resolves the repo from git remotes, and this clone also has an 'upstream'
rem remote pointing at moonlight-stream/moonlight-qt, which gh picks in preference.
rem Every gh call therefore has to name the repo explicitly, or releases get checked
rem for (and attempted against) upstream instead of our fork.
set REPO=Totaie/umbra

set ARCH=x64
set VERSION_FILE=%SOURCE_ROOT%\app\version.txt

if "%DRYRUN%"=="0" (
    where gh >nul 2>&1
    if !ERRORLEVEL! NEQ 0 (
        echo The GitHub CLI ^(gh^) is required to publish.
        echo Install it from https://cli.github.com and run: gh auth login
        exit /b 1
    )
)

rem --- version ---------------------------------------------------------------
set /p VERSION=<"%VERSION_FILE%"
for /f "tokens=1,2,3 delims=." %%a in ("%VERSION%") do (
    set MAJOR=%%a
    set MINOR=%%b
    set PATCH=%%c
)
if "%PATCH%"=="" (
    echo app\version.txt should hold a three part version like 1.2.3, found '%VERSION%'.
    exit /b 1
)

if not "%BUMP%"=="" (
    if "%BUMP%"=="major" set /a MAJOR=!MAJOR!+1 & set MINOR=0 & set PATCH=0
    if "%BUMP%"=="minor" set /a MINOR=!MINOR!+1 & set PATCH=0
    if "%BUMP%"=="patch" set /a PATCH=!PATCH!+1
    set VERSION=!MAJOR!.!MINOR!.!PATCH!
    rem NB: no trailing newline. app.pro reads this with $$cat() straight into a
    rem compiler define, and a stray newline breaks the version string.
    <nul set /p="!VERSION!" > "%VERSION_FILE%"
    echo Version bumped to !VERSION!
)

set TAG=v!VERSION!

rem Refuse to clobber an existing release rather than silently doing nothing.
if "%DRYRUN%"=="0" (
    gh release view "!TAG!" --repo %REPO% >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        echo Release !TAG! already exists. Use bump-patch, or delete the existing release.
        exit /b 1
    )
)

rem --- host ------------------------------------------------------------------
rem Built first, as this same version, so the installer below bundles exactly this
rem host instead of whichever one was published last.
set HOST_ASSET=
set HOST_ASSET_NAME=
if "%NOHOST%"=="1" goto host_done

if not exist "%SOURCE_ROOT%\host\third-party\Simple-Web-Server\server_http.hpp" (
    echo Fetching the host's submodules ^(only the first time; this takes a while^)...
    call "%SOURCE_ROOT%\scripts\fetch-host-submodules.bat"
    if !ERRORLEVEL! NEQ 0 exit /b 1
)

echo.
echo Building Umbra Host !VERSION!...
set UMBRA_BUILD_VERSION=!VERSION!
call "%SOURCE_ROOT%\host\scripts\umbra-build.bat" package
if !ERRORLEVEL! NEQ 0 (
    echo Host build failed.
    exit /b 1
)

rem cpack writes the installer as <CPACK_PACKAGE_FILE_NAME>.exe. Read that name from
rem the generated config rather than scanning for the newest .exe, because sunshine.exe
rem and uninstall.exe also live in host\build and would be picked instead.
set HOST_BUILD=%SOURCE_ROOT%\host\build
set PKG_BASE=
for /f "usebackq tokens=2 delims= " %%v in (`findstr /c:"CPACK_PACKAGE_FILE_NAME " "!HOST_BUILD!\CPackConfig.cmake"`) do (
    if not defined PKG_BASE set PKG_BASE=%%~v
)
set PKG_BASE=!PKG_BASE:"=!
set PKG_BASE=!PKG_BASE:)=!
set HOST_PACKAGE=!HOST_BUILD!\cpack_artifacts\!PKG_BASE!.exe
if not exist "!HOST_PACKAGE!" set HOST_PACKAGE=!HOST_BUILD!\!PKG_BASE!.exe
if not exist "!HOST_PACKAGE!" (
    echo cpack completed but !PKG_BASE!.exe was not found in host\build\cpack_artifacts or host\build.
    exit /b 1
)

rem An installer older than the binary means packaging didn't run this time and the
rem file is a previous build's - which is how host v0.2.7 once shipped as v0.2.6.
powershell -NoProfile -Command "exit [int]((Get-Item '!HOST_PACKAGE!').LastWriteTime -lt (Get-Item '!HOST_BUILD!\sunshine.exe').LastWriteTime)"
if !ERRORLEVEL! EQU 1 (
    echo.
    echo REFUSING TO PUBLISH: the host installer is older than sunshine.exe, so packaging
    echo did not run for this build. Re-run and check that cpack produced a new installer.
    exit /b 1
)

set HOST_ASSET_NAME=UmbraHostSetup-%ARCH%-!VERSION!.exe
if not exist "%SOURCE_ROOT%\build\host-release" mkdir "%SOURCE_ROOT%\build\host-release"
set HOST_ASSET=%SOURCE_ROOT%\build\host-release\!HOST_ASSET_NAME!
copy /y "!HOST_PACKAGE!" "!HOST_ASSET!" >nul

rem umbra-installer.bat bundles this one instead of downloading a published host.
set UMBRA_LOCAL_HOST=!HOST_ASSET!
set UMBRA_LOCAL_HOST_VERSION=!VERSION!
:host_done

rem --- build -----------------------------------------------------------------
echo.
echo Building Umbra !VERSION!...
if "%NOHOST%"=="1" (
    call "%SOURCE_ROOT%\scripts\umbra-installer.bat" release --no-host
) else (
    call "%SOURCE_ROOT%\scripts\umbra-installer.bat" release
)
if !ERRORLEVEL! NEQ 0 (
    echo Build failed.
    exit /b 1
)

set BUILT=%SOURCE_ROOT%\build\installer-%ARCH%-release\UmbraSetup.exe
if not exist "!BUILT!" (
    echo Expected an installer at !BUILT! but none was produced.
    exit /b 1
)

rem Verify the binary actually carries the version we are about to tag. A stale
rem configure silently ships the previous version, which then disagrees with the
rem release, and the update checker compares the wrong number. Checking the linked
rem binary is the only way to know rather than assume.
set BUILT_VERSION=
for /f %%v in ('powershell -NoProfile -Command "(Get-Item '%SOURCE_ROOT%\build\deploy-%ARCH%-release\Umbra.exe').VersionInfo.ProductVersion"') do set BUILT_VERSION=%%v
if not "!BUILT_VERSION!"=="!VERSION!.0" (
    if not "!BUILT_VERSION!"=="!VERSION!" (
        echo.
        echo Version mismatch: Umbra.exe reports '!BUILT_VERSION!' but this release is '!VERSION!'.
        echo The build did not pick up app\version.txt. Re-run with a clean build:
        echo     scripts\umbra-build.bat release clean
        exit /b 1
    )
)
echo Verified Umbra.exe reports version !BUILT_VERSION!

rem The update checker matches assets on architecture, so it has to be in the name.
set ASSET_NAME=UmbraSetup-%ARCH%-!VERSION!.exe
set ASSET=%SOURCE_ROOT%\build\installer-%ARCH%-release\!ASSET_NAME!
copy /y "!BUILT!" "!ASSET!" >nul

rem ---------------------------------------------------------------------------
rem Refuse to publish something the local antivirus already objects to. The
rem bundle embeds the host installer, so a host release that Defender dislikes
rem shows up here as a WIX0001 "file contains a virus" build failure - which is
rem what happened with host v0.2.1. Catch it as a clear message instead.
rem ---------------------------------------------------------------------------
set MPCMDRUN=%ProgramFiles%\Windows Defender\MpCmdRun.exe
if exist "!MPCMDRUN!" (
    for %%a in ("!ASSET!" "!HOST_ASSET!") do if not "%%~a"=="" (
        echo Scanning %%~nxa before publishing...
        "!MPCMDRUN!" -Scan -ScanType 3 -File "%%~a" -DisableRemediation >nul 2>&1
        if !ERRORLEVEL! EQU 2 (
            echo.
            echo REFUSING TO PUBLISH: Windows Defender flags %%~nxa.
            echo Run this to see the detection:
            echo   "!MPCMDRUN!" -Scan -ScanType 3 -File "%%~a" -DisableRemediation
            exit /b 1
        )
        echo   clean
    )
) else (
    echo Windows Defender not found; skipping the pre-publish scan.
)

for %%f in ("!ASSET!") do set ASSET_SIZE=%%~zf
set SHA=
for /f "usebackq skip=1 tokens=*" %%h in (`certutil -hashfile "!ASSET!" SHA256`) do (
    if not defined SHA set SHA=%%h
)

echo.
echo Built !ASSET_NAME! ^(!ASSET_SIZE! bytes^)
echo   sha256 !SHA!

set HOST_ASSET_SIZE=
set HOST_SHA=
if defined HOST_ASSET (
    for %%f in ("!HOST_ASSET!") do set HOST_ASSET_SIZE=%%~zf
    for /f "usebackq skip=1 tokens=*" %%h in (`certutil -hashfile "!HOST_ASSET!" SHA256`) do (
        if not defined HOST_SHA set HOST_SHA=%%h
    )
    echo Built !HOST_ASSET_NAME! ^(!HOST_ASSET_SIZE! bytes^)
    echo   sha256 !HOST_SHA!
)

if "%DRYRUN%"=="1" (
    echo.
    echo Dry run: not publishing. Assets staged at
    echo   !ASSET!
    if defined HOST_ASSET echo   !HOST_ASSET!
    exit /b 0
)

rem --- publish ---------------------------------------------------------------
rem The release is tagged at the commit that was built, and that commit has to be on
rem GitHub: the Arch package workflow builds the Linux package from the tag, so Umbra
rem on Arch updates from the same release, and the same source, as Windows. Without
rem --target, gh tags whatever the default branch happens to be - upstream's master.
rem A bumped version.txt is committed first, so the tagged commit says the version.
if not "%BUMP%"=="" (
    git -C "%SOURCE_ROOT%" commit -m "Bump version to !VERSION!" -- app/version.txt
    if !ERRORLEVEL! NEQ 0 (
        echo Could not commit app\version.txt.
        exit /b 1
    )
)
git -C "%SOURCE_ROOT%" diff --quiet --ignore-submodules HEAD
if !ERRORLEVEL! NEQ 0 (
    echo.
    echo REFUSING TO PUBLISH: this checkout has uncommitted changes, so the installer was
    echo built from source no commit holds, and the Arch package built from the tag would
    echo differ from it. Commit or stash them and run this again.
    exit /b 1
)
for /f "usebackq tokens=*" %%c in (`git -C "%SOURCE_ROOT%" rev-parse HEAD`) do set COMMIT=%%c
for /f "usebackq tokens=*" %%b in (`git -C "%SOURCE_ROOT%" rev-parse --abbrev-ref HEAD`) do set BRANCH=%%b
echo Pushing !BRANCH! ^(!COMMIT!^) so the release can be tagged there...
git -C "%SOURCE_ROOT%" push origin HEAD:refs/heads/!BRANCH!
if !ERRORLEVEL! NEQ 0 (
    echo Could not push !BRANCH! to GitHub.
    exit /b 1
)

set NOTES=%TEMP%\umbra-release-notes-!VERSION!.md
> "!NOTES!" echo Umbra !VERSION!
>>"!NOTES!" echo.
>>"!NOTES!" echo Installer: `!ASSET_NAME!` ^(!ASSET_SIZE! bytes^)
>>"!NOTES!" echo SHA256: `!SHA!`
>>"!NOTES!" echo.
if defined HOST_ASSET (
    >>"!NOTES!" echo Umbra Host !VERSION! is bundled, and on its own as `!HOST_ASSET_NAME!` ^(!HOST_ASSET_SIZE! bytes^)
    >>"!NOTES!" echo SHA256: `!HOST_SHA!`
    >>"!NOTES!" echo.
)
>>"!NOTES!" echo Existing installs offer this update automatically. Builds are unsigned,
>>"!NOTES!" echo so Windows SmartScreen warns the first time you run the installer.
>>"!NOTES!" echo.
>>"!NOTES!" echo The Arch Linux package is attached a few minutes after publishing, by the
>>"!NOTES!" echo Arch package workflow, built from this same commit.

set PRERELEASE_FLAG=
if "%PRERELEASE%"=="1" set PRERELEASE_FLAG=--prerelease
set ASSETS="!ASSET!"
if defined HOST_ASSET set ASSETS=!ASSETS! "!HOST_ASSET!"

echo.
echo Publishing !TAG!...
gh release create "!TAG!" !ASSETS! --repo %REPO% --target !COMMIT! --title "Umbra !VERSION!" --notes-file "!NOTES!" !PRERELEASE_FLAG!
if !ERRORLEVEL! NEQ 0 (
    del /q "!NOTES!" 2>nul
    echo gh release create failed.
    exit /b 1
)
del /q "!NOTES!" 2>nul

rem --- hosts still looking at Totaie/umbra-host ------------------------------
rem Hosts from before the move check Totaie/umbra-host for updates, so each host
rem installer goes there too, under the same tag. The host it installs looks here
rem from then on. Archive that repository once no host needs it, and this stops.
if not defined HOST_ASSET goto bridge_done
set BRIDGE_REPO=Totaie/umbra-host
set BRIDGE_ARCHIVED=
for /f "usebackq tokens=*" %%a in (`gh repo view !BRIDGE_REPO! --json isArchived --jq .isArchived 2^>nul`) do set BRIDGE_ARCHIVED=%%a
if not "!BRIDGE_ARCHIVED!"=="false" goto bridge_done
echo.
echo Copying the host installer to !BRIDGE_REPO! for hosts that still look there...
gh release create "!TAG!" "!HOST_ASSET!" --repo !BRIDGE_REPO! --title "Umbra Host !VERSION!" --notes "Umbra Host is now released with Umbra, from https://github.com/Totaie/umbra. This copy is for hosts that still look here for updates; the one it installs looks there." !PRERELEASE_FLAG!
if !ERRORLEVEL! NEQ 0 (
    echo   That failed. The release on %REPO% is fine, but hosts older than this
    echo   one won't be offered it until it is copied there.
)
:bridge_done

echo.
echo Published !TAG!
if "%PRERELEASE%"=="1" (
    echo Marked as a prerelease, so it will NOT be offered as an update until promoted.
)
if not "%BUMP%"=="" (
    echo app\version.txt was committed and pushed with the release.
)
exit /b 0
