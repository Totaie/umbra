@echo off
setlocal enableDelayedExpansion

rem ---------------------------------------------------------------------------
rem Umbra developer build script
rem
rem Unlike scripts\build-arch.bat (the upstream release pipeline, which needs a
rem Qt command prompt, WiX, a code signing cert and 7-Zip), this script is meant
rem for iterating: run it from any shell and it finds Qt and Visual Studio on its
rem own, then produces a runnable .exe with all its DLLs next to it.
rem
rem Usage:
rem   scripts\umbra-build.bat [release^|debug] [clean]
rem
rem Override autodetection with UMBRA_QT_DIR, e.g.
rem   set UMBRA_QT_DIR=D:\Qt\6.11.1\msvc2022_64
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

set DO_CLEAN=0
if /I "%~2"=="clean" set DO_CLEAN=1

set SOURCE_ROOT=%~dp0..
pushd %SOURCE_ROOT%
set SOURCE_ROOT=%cd%
popd

set ARCH=x64
set BUILD_ROOT=%SOURCE_ROOT%\build
set BUILD_FOLDER=%BUILD_ROOT%\build-%ARCH%-%BUILD_CONFIG%
set DEPLOY_FOLDER=%BUILD_ROOT%\deploy-%ARCH%-%BUILD_CONFIG%

rem ---------------------------------------------------------------------------
rem Locate Qt
rem ---------------------------------------------------------------------------
if defined UMBRA_QT_DIR (
    set QT_DIR=%UMBRA_QT_DIR%
) else (
    call :FindQt
)

if not exist "%QT_DIR%\bin\qmake.exe" (
    echo Unable to find qmake.exe under "%QT_DIR%".
    echo Install Qt 6.11+ for MSVC, or point UMBRA_QT_DIR at your Qt kit, e.g.
    echo   set UMBRA_QT_DIR=D:\Qt\6.11.1\msvc2022_64
    exit /b 1
)
echo Using Qt at %QT_DIR%
set PATH=%QT_DIR%\bin;%PATH%

rem ---------------------------------------------------------------------------
rem Locate Visual Studio and enter the x64 native toolchain
rem ---------------------------------------------------------------------------
set VSWHERE="%SOURCE_ROOT%\scripts\vswhere.exe"
set VS_PATH=
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS_PATH=%%i
if "%VS_PATH%"=="" (
    echo Unable to find a Visual Studio install with the C++ toolchain.
    echo Install the "Desktop development with C++" workload.
    exit /b 1
)
echo Using Visual Studio at %VS_PATH%
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if !ERRORLEVEL! NEQ 0 (
    echo Failed to initialize the MSVC environment.
    exit /b 1
)

rem ---------------------------------------------------------------------------
rem Third-party prebuilt dependencies (FFmpeg, SDL, OpenSSL, ...)
rem ---------------------------------------------------------------------------
if not exist "%SOURCE_ROOT%\libs\windows\lib\%ARCH%" (
    echo Fetching prebuilt dependencies...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SOURCE_ROOT%\setup-deps.ps1"
    if !ERRORLEVEL! NEQ 0 exit /b 1
)

rem ---------------------------------------------------------------------------
rem Configure and compile
rem ---------------------------------------------------------------------------
if "%DO_CLEAN%"=="1" (
    echo Cleaning %BUILD_FOLDER%
    if exist "%BUILD_FOLDER%" rmdir /s /q "%BUILD_FOLDER%"
    if exist "%DEPLOY_FOLDER%" rmdir /s /q "%DEPLOY_FOLDER%"
)
if not exist "%BUILD_FOLDER%" mkdir "%BUILD_FOLDER%"
if not exist "%DEPLOY_FOLDER%" mkdir "%DEPLOY_FOLDER%"

echo Configuring...
pushd "%BUILD_FOLDER%"
qmake.exe "%SOURCE_ROOT%\umbra.pro"
if !ERRORLEVEL! NEQ 0 (
    popd
    goto Error
)

echo Compiling Umbra ^(%BUILD_CONFIG%^)...
"%SOURCE_ROOT%\scripts\jom.exe" %BUILD_CONFIG%
if !ERRORLEVEL! NEQ 0 (
    popd
    goto Error
)
popd

rem ---------------------------------------------------------------------------
rem Stage a runnable tree
rem ---------------------------------------------------------------------------
echo Staging runtime dependencies...
copy /y "%SOURCE_ROOT%\libs\windows\lib\%ARCH%\*.dll" "%DEPLOY_FOLDER%" >nul
if !ERRORLEVEL! NEQ 0 goto Error

copy /y "%BUILD_FOLDER%\AntiHooking\%BUILD_CONFIG%\AntiHooking.dll" "%DEPLOY_FOLDER%" >nul
if !ERRORLEVEL! NEQ 0 goto Error

copy /y "%SOURCE_ROOT%\app\SDL_GameControllerDB\gamecontrollerdb.txt" "%DEPLOY_FOLDER%" >nul
if !ERRORLEVEL! NEQ 0 goto Error

copy /y "%BUILD_FOLDER%\app\%BUILD_CONFIG%\Umbra.exe" "%DEPLOY_FOLDER%" >nul
if !ERRORLEVEL! NEQ 0 goto Error

echo Deploying Qt runtime...
set WINDEPLOYQT_ARGS=--no-system-d3d-compiler --no-system-dxc-compiler --skip-plugin-types qmltooling,generic --no-ffmpeg
set WINDEPLOYQT_ARGS=!WINDEPLOYQT_ARGS! --no-quickcontrols2fusion --no-quickcontrols2imagine --no-quickcontrols2universal
set WINDEPLOYQT_ARGS=!WINDEPLOYQT_ARGS! --no-quickcontrols2fusionstyleimpl --no-quickcontrols2imaginestyleimpl --no-quickcontrols2universalstyleimpl --no-quickcontrols2windowsstyleimpl --no-quickcontrols2fluentwinui3styleimpl
windeployqt.exe --dir "%DEPLOY_FOLDER%" --%BUILD_CONFIG% --qmldir "%SOURCE_ROOT%\app\gui" --no-opengl-sw --no-compiler-runtime --no-sql !WINDEPLOYQT_ARGS! "%DEPLOY_FOLDER%\Umbra.exe"
if !ERRORLEVEL! NEQ 0 goto Error

rem Mark this as a portable build so it keeps its settings beside the exe and
rem does not collide with an installed Umbra or Moonlight.
echo. > "%DEPLOY_FOLDER%\portable.dat"

echo.
echo Build successful.
echo   %DEPLOY_FOLDER%\Umbra.exe
exit /b 0

rem ---------------------------------------------------------------------------
:FindQt
rem Pick the highest-versioned MSVC x64 kit we can find. Directory enumeration is
rem lexical, so 6.9.0 would beat 6.11.1; compare version numbers properly.
set QT_DIR=
set BEST_KEY=0
for %%r in (D:\Qt C:\Qt %USERPROFILE%\Qt) do (
    if exist "%%r" (
        for /d %%v in ("%%r\6.*") do (
            for /d %%k in ("%%v\msvc*_64") do (
                if exist "%%k\bin\qmake.exe" (
                    call :VersionKey "%%~nxv"
                    if !VER_KEY! GTR !BEST_KEY! (
                        set BEST_KEY=!VER_KEY!
                        set QT_DIR=%%k
                    )
                )
            )
        )
    )
)
exit /b 0

:VersionKey
rem Turn 6.11.1 into a sortable integer: 6*1000000 + 11*1000 + 1
set VER_STR=%~1
for /f "tokens=1,2,3 delims=." %%a in ("%VER_STR%") do (
    set /a VER_KEY=%%a*1000000 + %%b*1000 + %%c 2>nul
)
if not defined VER_KEY set VER_KEY=0
exit /b 0

:Error
echo.
echo Build failed.
exit /b 1
