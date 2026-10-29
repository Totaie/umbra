@echo off
setlocal

rem ---------------------------------------------------------------------------
rem Fetches the submodules Umbra Host (host\) builds with.
rem
rem .gitmodules marks them "update = none", so that a client checkout - and the
rem client's CI, and a plain "git submodule update --init --recursive" - doesn't
rem download all of them. This asks for them by name. Safe to run again: it only
rem fetches what is missing or not at the recorded commit.
rem
rem Usage:
rem   scripts\fetch-host-submodules.bat
rem
rem scripts\publish-release.bat runs it by itself the first time.
rem ---------------------------------------------------------------------------

set SOURCE_ROOT=%~dp0..
pushd "%SOURCE_ROOT%"

rem Some of these nest deep enough to pass Windows' 260 character path limit.
git -c core.longpaths=true submodule update --init --checkout --recursive -- host
set RESULT=%ERRORLEVEL%

popd
if not "%RESULT%"=="0" (
    echo Fetching the host's submodules failed.
    exit /b 1
)
exit /b 0
