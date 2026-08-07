@echo off
setlocal enableDelayedExpansion

rem ---------------------------------------------------------------------------
rem Builds Umbra and publishes the installer to GitHub Releases.
rem
rem Umbra updates itself from GitHub Releases: AutoUpdateChecker reads
rem /releases/latest and looks for an installer asset whose name contains the
rem running machine's architecture. This script produces exactly that.
rem
rem app\version.txt is the source of truth. The tag is v<version> and the asset is
rem UmbraSetup-<arch>-<version>.exe, so one release can carry x64 and ARM64.
rem
rem Usage:
rem   scripts\publish-release.bat [options]
rem
rem   bump-patch      bump the third version component before building
rem   bump-minor      bump the second, resetting patch
rem   bump-major      bump the first, resetting the rest
rem   prerelease      publish as a prerelease, so it is NOT offered as an update
rem   dry-run         build and stage the asset without touching GitHub
rem   no-host         build a client-only installer
rem
rem Requires the GitHub CLI (gh), authenticated. Builds are unsigned, so Windows
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
    echo Scanning the installer before publishing...
    "!MPCMDRUN!" -Scan -ScanType 3 -File "!ASSET!" -DisableRemediation >nul 2>&1
    if !ERRORLEVEL! EQU 2 (
        echo.
        echo REFUSING TO PUBLISH: Windows Defender flags !ASSET_NAME!.
        echo Run this to see the detection:
        echo   "!MPCMDRUN!" -Scan -ScanType 3 -File "!ASSET!" -DisableRemediation
        exit /b 1
    )
    echo   clean
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

if "%DRYRUN%"=="1" (
    echo.
    echo Dry run: not publishing. Asset staged at
    echo   !ASSET!
    exit /b 0
)

rem --- publish ---------------------------------------------------------------
set NOTES=%TEMP%\umbra-release-notes-!VERSION!.md
> "!NOTES!" echo Umbra !VERSION!
>>"!NOTES!" echo.
>>"!NOTES!" echo Installer: `!ASSET_NAME!` ^(!ASSET_SIZE! bytes^)
>>"!NOTES!" echo SHA256: `!SHA!`
>>"!NOTES!" echo.
>>"!NOTES!" echo Existing installs offer this update automatically. Builds are unsigned,
>>"!NOTES!" echo so Windows SmartScreen warns the first time you run the installer.

echo.
echo Publishing !TAG!...
if "%PRERELEASE%"=="1" (
    gh release create "!TAG!" "!ASSET!" --repo %REPO% --title "Umbra !VERSION!" --notes-file "!NOTES!" --prerelease
) else (
    gh release create "!TAG!" "!ASSET!" --repo %REPO% --title "Umbra !VERSION!" --notes-file "!NOTES!"
)
if !ERRORLEVEL! NEQ 0 (
    del /q "!NOTES!" 2>nul
    echo gh release create failed.
    exit /b 1
)
del /q "!NOTES!" 2>nul

echo.
echo Published !TAG!
if "%PRERELEASE%"=="1" (
    echo Marked as a prerelease, so it will NOT be offered as an update until promoted.
)
if not "%BUMP%"=="" (
    echo Remember to commit and push app\version.txt.
)
exit /b 0
