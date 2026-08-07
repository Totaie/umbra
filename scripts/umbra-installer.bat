@echo off
setlocal enableDelayedExpansion

rem ---------------------------------------------------------------------------
rem Builds the Umbra installer: the MSI, plus the bundle that chains the VC++
rem redistributable and (optionally) the Umbra host.
rem
rem Usage:
rem   scripts\umbra-installer.bat [release^|debug] [--no-host]
rem
rem Runs scripts\umbra-build.bat first, so this is the single command that goes
rem from a clean checkout to a shippable UmbraSetup.exe.
rem ---------------------------------------------------------------------------

set BUILD_CONFIG=%~1
if "%BUILD_CONFIG%"=="" set BUILD_CONFIG=release
if /I "%BUILD_CONFIG%"=="release" (
    set BUILD_CONFIG=release
) else (
    if /I "%BUILD_CONFIG%"=="debug" (
        set BUILD_CONFIG=debug
    ) else (
        echo Invalid build configuration '%BUILD_CONFIG%' - expected 'release' or 'debug'
        exit /b 1
    )
)

set WITH_HOST=1
if /I "%~2"=="--no-host" set WITH_HOST=0

set SOURCE_ROOT=%~dp0..
pushd %SOURCE_ROOT%
set SOURCE_ROOT=%cd%
popd

set ARCH=x64
set BUILD_ROOT=%SOURCE_ROOT%\build
set BUILD_FOLDER=%BUILD_ROOT%\build-%ARCH%-%BUILD_CONFIG%
set DEPLOY_FOLDER=%BUILD_ROOT%\deploy-%ARCH%-%BUILD_CONFIG%
set STAGE_FOLDER=%BUILD_ROOT%\wixstage-%ARCH%-%BUILD_CONFIG%
set INSTALLER_FOLDER=%BUILD_ROOT%\installer-%ARCH%-%BUILD_CONFIG%

rem ---------------------------------------------------------------------------
rem Build the application first
rem ---------------------------------------------------------------------------
call "%SOURCE_ROOT%\scripts\umbra-build.bat" %BUILD_CONFIG%
if !ERRORLEVEL! NEQ 0 (
    echo Application build failed.
    exit /b 1
)

rem ---------------------------------------------------------------------------
rem Stage the payload WiX harvests.
rem
rem This is everything in the deploy tree EXCEPT Umbra.exe (installed explicitly
rem by Product.wxs, so harvesting it too would collide on a generated component
rem GUID) and portable.dat (which would make an installed Umbra store settings in
rem Program Files). Local test runs also leave logs and a settings folder behind,
rem which must not ship.
rem ---------------------------------------------------------------------------
echo Staging installer payload...
if exist "%STAGE_FOLDER%" rmdir /s /q "%STAGE_FOLDER%"
mkdir "%STAGE_FOLDER%"

robocopy "%DEPLOY_FOLDER%" "%STAGE_FOLDER%" /E /NJH /NJS /NP /NDL /NFL ^
    /XF Umbra.exe portable.dat portable.dat.inactive Umbra-*.log ^
    /XD "Umbra Project" >nul
rem robocopy uses exit codes 0-7 for success
if !ERRORLEVEL! GEQ 8 (
    echo Failed to stage installer payload.
    exit /b 1
)

if exist "%STAGE_FOLDER%\Umbra.exe" (
    echo Staging error: Umbra.exe must not be in the harvested payload.
    exit /b 1
)

rem ---------------------------------------------------------------------------
rem Fetch the host installer chained by the bundle
rem ---------------------------------------------------------------------------
if "%WITH_HOST%"=="1" (
    rem The cache is only usable if it holds the newest host release. Keying it on
    rem the file existing was enough to make a release ship a months-old host with
    rem no warning, so the tag that was downloaded is compared against the tag that
    rem is current. A failed lookup (offline, gh not logged in) falls back to the
    rem cache rather than blocking the build.
    set HOST_CACHE_TAG=
    if exist "%BUILD_ROOT%\host\TAG.txt" (
        for /f "usebackq delims=" %%t in ("%BUILD_ROOT%\host\TAG.txt") do set HOST_CACHE_TAG=%%t
    )

    set HOST_LATEST_TAG=
    for /f "usebackq delims=" %%t in (`gh release list --repo Totaie/umbra-host --limit 1 --json tagName --jq ".[0].tagName" 2^>nul`) do (
        if not defined HOST_LATEST_TAG set HOST_LATEST_TAG=%%t
    )

    set FETCH_HOST=0
    if not exist "%BUILD_ROOT%\host\UmbraHostSetup.exe" set FETCH_HOST=1
    if not "!HOST_LATEST_TAG!"=="" if not "!HOST_CACHE_TAG!"=="!HOST_LATEST_TAG!" set FETCH_HOST=1

    if "!FETCH_HOST!"=="1" (
        if not "!HOST_CACHE_TAG!"=="" echo Cached host is !HOST_CACHE_TAG!, newest is !HOST_LATEST_TAG!.
        echo Fetching Umbra host installer...
        call "%SOURCE_ROOT%\scripts\fetch-host.bat"
        if !ERRORLEVEL! NEQ 0 (
            echo Failed to fetch the host installer.
            echo Re-run with --no-host to build a client-only installer.
            exit /b 1
        )
    ) else (
        echo Using cached !HOST_CACHE_TAG! host installer at %BUILD_ROOT%\host\UmbraHostSetup.exe
    )

    rem Bundle.wxs compares this against the installed sunshine.exe's file version to
    rem decide whether the host needs upgrading, so it has to be the plain numeric
    rem version the host stamps into its binary - the tag without its leading "v".
    set UMBRA_HOST_VERSION=
    if exist "%BUILD_ROOT%\host\TAG.txt" (
        for /f "usebackq delims=" %%t in ("%BUILD_ROOT%\host\TAG.txt") do set UMBRA_HOST_VERSION=%%t
    )
    if "!UMBRA_HOST_VERSION:~0,1!"=="v" set UMBRA_HOST_VERSION=!UMBRA_HOST_VERSION:~1!
    if "!UMBRA_HOST_VERSION!"=="" (
        echo Could not determine the bundled host version from TAG.txt.
        echo Delete %BUILD_ROOT%\host and re-run so it is fetched again.
        exit /b 1
    )
    echo Bundling Umbra Host !UMBRA_HOST_VERSION!
)

rem ---------------------------------------------------------------------------
rem Build the MSI and bundle. WixToolset.Sdk comes from NuGet via -Restore, so no
rem separate WiX Toolset install is needed.
rem ---------------------------------------------------------------------------
set VSWHERE="%SOURCE_ROOT%\scripts\vswhere.exe"
set MSBUILD=
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if "%MSBUILD%"=="" (
    echo Unable to find MSBuild. Install the Visual Studio build tools.
    exit /b 1
)

if not exist "%INSTALLER_FOLDER%" mkdir "%INSTALLER_FOLDER%"

rem Umbra.wixproj passes DeployDir=$(DEPLOY_FOLDER) and BuildDir=$(BUILD_FOLDER)
rem through to the compiler, and MSBuild picks both up from the environment.
rem Point DEPLOY_FOLDER at the staged payload rather than the runnable tree.
set DEPLOY_FOLDER=%STAGE_FOLDER%

echo Building MSI...
cmd /c "set VERSION= && "%MSBUILD%" -Restore "%SOURCE_ROOT%\wix\Umbra\Umbra.wixproj" /p:Configuration=%BUILD_CONFIG% /p:Platform=%ARCH% /p:MSBuildProjectExtensionsPath=%BUILD_FOLDER%\"
if !ERRORLEVEL! NEQ 0 goto Error

echo MSI built: %BUILD_FOLDER%\Umbra.msi

rem ---------------------------------------------------------------------------
rem Build the bundle. This script only produces an x64 MSI, so skip the ARM64
rem package; scripts\build-arch.bat builds both for a real release.
rem ---------------------------------------------------------------------------
set UMBRA_SKIP_ARM64=1

if "%WITH_HOST%"=="0" (
    echo Building client-only bundle...
    set UMBRA_SKIP_HOST=1
) else (
    echo Building bundle with the Umbra host...
)

echo Building bundle...
cmd /c "set VERSION= && "%MSBUILD%" -Restore "%SOURCE_ROOT%\wix\UmbraSetup\UmbraSetup.wixproj" /p:Configuration=%BUILD_CONFIG% /p:Platform=%ARCH% /p:MSBuildProjectExtensionsPath=%BUILD_FOLDER%\"
if !ERRORLEVEL! NEQ 0 goto Error

if exist "%BUILD_FOLDER%\UmbraSetup.exe" (
    copy /y "%BUILD_FOLDER%\UmbraSetup.exe" "%INSTALLER_FOLDER%" >nul
)

echo.
echo Installer built:
echo   %BUILD_FOLDER%\Umbra.msi
echo   %INSTALLER_FOLDER%\UmbraSetup.exe
exit /b 0

:Error
echo.
echo Installer build failed.
exit /b 1
