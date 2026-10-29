@echo off
setlocal enableDelayedExpansion

rem ---------------------------------------------------------------------------
rem Builds the Umbra host from a normal Windows command prompt.
rem
rem The host has to be compiled inside MSYS2 UCRT64 (that's upstream's toolchain,
rem not a choice we get to make), so this is a wrapper that shells into it and runs
rem scripts\umbra_windows_build.sh. Run it from anywhere; no MSYS2 shell needed.
rem
rem Usage:
rem   scripts\umbra-build.bat [clean] [package]
rem
rem   clean     wipe the build directory first
rem   package   also produce the NSIS installer
rem
rem Override the MSYS2 location with UMBRA_MSYS2_ROOT if it isn't at C:\msys64.
rem ---------------------------------------------------------------------------

set SOURCE_ROOT=%~dp0..
pushd "%SOURCE_ROOT%"
set SOURCE_ROOT=%cd%
popd

set SH_ARGS=
:parse
if "%~1"=="" goto parsed
if /I "%~1"=="clean"   set SH_ARGS=!SH_ARGS! --clean
if /I "%~1"=="package" set SH_ARGS=!SH_ARGS! --package
shift
goto parse
:parsed

set MSYS2_ROOT=%UMBRA_MSYS2_ROOT%
if "%MSYS2_ROOT%"=="" set MSYS2_ROOT=C:\msys64

if not exist "%MSYS2_ROOT%\usr\bin\bash.exe" (
    echo MSYS2 was not found at %MSYS2_ROOT%.
    echo.
    echo Install it from https://www.msys2.org, then from an "MSYS2 UCRT64" shell run:
    echo   pacman -Syu
    echo   pacman -S git mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake ^
mingw-w64-ucrt-x86_64-boost mingw-w64-ucrt-x86_64-cppwinrt ^
mingw-w64-ucrt-x86_64-curl-winssl mingw-w64-ucrt-x86_64-miniupnpc ^
mingw-w64-ucrt-x86_64-onevpl mingw-w64-ucrt-x86_64-openssl ^
mingw-w64-ucrt-x86_64-opus mingw-w64-ucrt-x86_64-MinHook ^
mingw-w64-ucrt-x86_64-nsis mingw-w64-ucrt-x86_64-nodejs ^
mingw-w64-ucrt-x86_64-nlohmann-json
    echo.
    echo Or set UMBRA_MSYS2_ROOT if MSYS2 is installed elsewhere.
    exit /b 1
)

rem Convert D:\path\to\repo into /d/path/to/repo for the MSYS2 side
set UNIX_ROOT=%SOURCE_ROOT:\=/%
set DRIVE_LETTER=%UNIX_ROOT:~0,1%
set UNIX_ROOT=/%DRIVE_LETTER%%UNIX_ROOT:~2%

echo Building the Umbra host via MSYS2 at %MSYS2_ROOT%...
echo.

rem Pin the version when the caller asked for one. build_version.cmake takes
rem PROJECT_VERSION from BUILD_VERSION (and requires BRANCH alongside it), and that
rem is the value the host's update check compares against release tags. Letting it
rem default while tagging a release something else makes every host believe it is
rem perpetually out of date.
set VERSION_EXPORT=
if not "%UMBRA_BUILD_VERSION%"=="" (
    set VERSION_EXPORT=export BRANCH=umbra-host-main; export BUILD_VERSION=%UMBRA_BUILD_VERSION%;
    echo Building as version %UMBRA_BUILD_VERSION%

    rem emit_windows_versioninfo derives a time-based FILEVERSION revision and refuses
    rem to emit one lower than what it cached, to avoid manufacturing a version that
    rem looks newer than it is. Switching version series leaves a much higher revision
    rem cached from the previous series, so a pinned build aborts with "Refusing to
    rem manufacture a future FILEVERSION". The cache is derived state, so drop it.
    if exist "%SOURCE_ROOT%\build\generated_versioninfo" (
        echo Clearing cached version info from the previous version series...
        rmdir /s /q "%SOURCE_ROOT%\build\generated_versioninfo"
    )
)

rem MSYSTEM must be set before /etc/profile is sourced, or the UCRT64 toolchain
rem won't be on PATH.
"%MSYS2_ROOT%\usr\bin\bash.exe" -lc "export MSYSTEM=UCRT64; source /etc/profile; %VERSION_EXPORT% cd '%UNIX_ROOT%' && ./scripts/umbra_windows_build.sh%SH_ARGS%"
if !ERRORLEVEL! NEQ 0 (
    echo.
    echo Build failed.
    exit /b 1
)

echo.
echo Build complete. Binaries are in %SOURCE_ROOT%\build
exit /b 0
