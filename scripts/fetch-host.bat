@echo off
setlocal enableDelayedExpansion

rem ---------------------------------------------------------------------------
rem Downloads the Umbra host installer that the Umbra bundle chains.
rem
rem The installer offers to install a host alongside the client so one machine can
rem both stream to other PCs and be streamed from. Rather than pinning Bundle.wxs
rem to a DownloadUrl with a baked-in size and SHA512 (which would need editing on
rem every host release), the payload is fetched here at build time and referenced
rem as a local file.
rem
rem Usage:
rem   scripts\fetch-host.bat [owner/repo] [tag]
rem
rem Defaults to the latest release of Nonary/vibepollo. Point it at
rem Totaie/umbra-host once that publishes its own installer.
rem
rem GPLv3: redistributing this binary in the Umbra installer obliges us to offer
rem the corresponding source. Totaie/umbra-host is the public fork satisfying that.
rem ---------------------------------------------------------------------------

set REPO=%~1
if "%REPO%"=="" set REPO=Nonary/vibepollo

set TAG=%~2

set SOURCE_ROOT=%~dp0..
pushd "%SOURCE_ROOT%"
set SOURCE_ROOT=%cd%
popd

set OUT_DIR=%SOURCE_ROOT%\build\host
set OUT_FILE=%OUT_DIR%\UmbraHostSetup.exe

where gh >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo The GitHub CLI ^(gh^) is required to download the host release.
    echo Install it from https://cli.github.com and run: gh auth login
    exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if exist "%OUT_FILE%" del /q "%OUT_FILE%"

if "%TAG%"=="" (
    echo Downloading the latest host installer from %REPO%...
    gh release download --repo "%REPO%" --pattern "*Setup*.exe" --output "%OUT_FILE%" --clobber
) else (
    echo Downloading the %TAG% host installer from %REPO%...
    gh release download "%TAG%" --repo "%REPO%" --pattern "*Setup*.exe" --output "%OUT_FILE%" --clobber
)
if !ERRORLEVEL! NEQ 0 (
    echo Download failed. If %REPO% is private, check that 'gh auth status' shows an
    echo account with access to it.
    exit /b 1
)

if not exist "%OUT_FILE%" (
    echo No installer matching *Setup*.exe was found in that release.
    exit /b 1
)

rem A truncated or empty download would fail confusingly at install time. The host
rem installer is tens of megabytes, so anything under a megabyte is broken.
for %%f in ("%OUT_FILE%") do set ACTUAL_SIZE=%%~zf
if !ACTUAL_SIZE! LSS 1000000 (
    del /q "%OUT_FILE%" 2>nul
    echo Downloaded file is only !ACTUAL_SIZE! bytes. Refusing to ship a truncated installer.
    exit /b 1
)

set SHA=
for /f "usebackq skip=1 tokens=*" %%h in (`certutil -hashfile "%OUT_FILE%" SHA256`) do (
    if not defined SHA set SHA=%%h
)

echo.
echo Host installer ready:
echo   %OUT_FILE%
echo   !ACTUAL_SIZE! bytes
echo   sha256 !SHA!
exit /b 0
