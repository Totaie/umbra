@echo off
setlocal enableDelayedExpansion

rem ---------------------------------------------------------------------------
rem Builds the Umbra host and publishes the installer to GitHub Releases.
rem
rem The host already updates itself: src/update.cpp polls the GitHub releases of
rem SUNSHINE_REPO_OWNER/SUNSHINE_REPO_NAME, which this fork points at
rem Totaie/umbra-host. This script produces the releases it reads, mirroring the
rem client's scripts\publish-release.bat.
rem
rem Usage:
rem   scripts\publish-release.bat [options]
rem
rem   prerelease   publish as a prerelease, so it is NOT offered as an update
rem   dry-run      build and package without touching GitHub
rem
rem Requires the GitHub CLI (gh), authenticated, and MSYS2 for the build itself.
rem
rem Builds are unsigned, so SmartScreen warns on first run. That lands harder for
rem the host than the client, because it installs a service and a display driver.
rem ---------------------------------------------------------------------------

rem Resolve our own location before parsing arguments: shift moves %0 too.
set SOURCE_ROOT=%~dp0..
pushd "%SOURCE_ROOT%"
set SOURCE_ROOT=%cd%
popd

set PRERELEASE=0
set DRYRUN=0
set VERSION=

:parse
if "%~1"=="" goto parsed
if /I "%~1"=="prerelease" (set PRERELEASE=1) else (
    if /I "%~1"=="dry-run" (set DRYRUN=1) else (
        set VERSION=%~1
    )
)
shift
goto parse
:parsed

if "%VERSION%"=="" (
    echo Usage: scripts\publish-release.bat ^<version^> [prerelease] [dry-run]
    echo   e.g. scripts\publish-release.bat 0.1.0 prerelease
    echo.
    echo The version is baked into the binary as PROJECT_VERSION and reused as the tag,
    echo so the host's update check compares like with like.
    exit /b 1
)

set ARCH=x64

if "%DRYRUN%"=="0" (
    where gh >nul 2>&1
    if !ERRORLEVEL! NEQ 0 (
        echo The GitHub CLI ^(gh^) is required to publish.
        echo Install it from https://cli.github.com and run: gh auth login
        exit /b 1
    )
)

rem --- build and package -----------------------------------------------------
rem Bake the release version into the binary so PROJECT_VERSION matches the tag we
rem are about to create. Without this the host compares its own version against a
rem differently-derived tag, concludes it is permanently out of date, and updates
rem in a loop.
set UMBRA_BUILD_VERSION=!VERSION!
call "%SOURCE_ROOT%\scripts\umbra-build.bat" package
if !ERRORLEVEL! NEQ 0 (
    echo Build failed.
    exit /b 1
)

rem cpack writes the installer as <CPACK_PACKAGE_FILE_NAME>.exe. Read that name from
rem the generated config rather than scanning for the newest .exe, because sunshine.exe
rem and uninstall.exe also live in build\ and would be picked instead.
set PKG_BASE=
for /f "usebackq tokens=2 delims= " %%v in (`findstr /c:"CPACK_PACKAGE_FILE_NAME " "%SOURCE_ROOT%\build\CPackConfig.cmake"`) do (
    if not defined PKG_BASE set PKG_BASE=%%~v
)
set PKG_BASE=!PKG_BASE:"=!
set PKG_BASE=!PKG_BASE:)=!

set PACKAGE=%SOURCE_ROOT%\build\!PKG_BASE!.exe
if not exist "!PACKAGE!" (
    echo cpack completed but !PACKAGE! was not produced.
    exit /b 1
)

set TAG=v!VERSION!

set ASSET_NAME=UmbraHostSetup-%ARCH%-!VERSION!.exe
set ASSET=%SOURCE_ROOT%\build\!ASSET_NAME!
copy /y "!PACKAGE!" "!ASSET!" >nul

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

gh release view "!TAG!" >nul 2>&1
if !ERRORLEVEL! EQU 0 (
    echo Release !TAG! already exists. Bump the project version or delete it first.
    exit /b 1
)

rem --- publish ---------------------------------------------------------------
set NOTES=%TEMP%\umbra-host-notes-!VERSION!.md
> "!NOTES!" echo Umbra Host !VERSION!
>>"!NOTES!" echo.
>>"!NOTES!" echo Installer: `!ASSET_NAME!` ^(!ASSET_SIZE! bytes^)
>>"!NOTES!" echo SHA256: `!SHA!`
>>"!NOTES!" echo.
>>"!NOTES!" echo Existing hosts pick this up on their next update check. Builds are
>>"!NOTES!" echo unsigned, so SmartScreen warns on first run.

echo.
echo Publishing !TAG!...
if "%PRERELEASE%"=="1" (
    gh release create "!TAG!" "!ASSET!" --title "Umbra Host !VERSION!" --notes-file "!NOTES!" --prerelease
) else (
    gh release create "!TAG!" "!ASSET!" --title "Umbra Host !VERSION!" --notes-file "!NOTES!"
)
if !ERRORLEVEL! NEQ 0 (
    del /q "!NOTES!" 2>nul
    echo gh release create failed.
    exit /b 1
)
del /q "!NOTES!" 2>nul

echo.
echo Published !TAG!
exit /b 0
