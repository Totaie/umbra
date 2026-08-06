@echo off
setlocal enableDelayedExpansion

rem ---------------------------------------------------------------------------
rem Installs the Qt SDK that Umbra builds against.
rem
rem Usage:
rem   scripts\setup-qt.bat [qt-version] [install-dir]
rem
rem Defaults to Qt 6.11.1 in D:\Qt. Wraps scripts\setup-qt.py, which exists because
rem aqtinstall cannot install any Qt 6.8+ MSVC kit on its own: it builds the
rem repository path qt6_6111/qt6_6111 instead of qt6_6111/qt6_6111_msvc2022_64, so
rem every metadata fetch 404s behind a misleading checksum error.
rem ---------------------------------------------------------------------------

set QT_VERSION=%~1
if "%QT_VERSION%"=="" set QT_VERSION=6.11.1

set QT_DIR=%~2
if "%QT_DIR%"=="" set QT_DIR=D:\Qt

set SOURCE_ROOT=%~dp0..
pushd "%SOURCE_ROOT%"
set SOURCE_ROOT=%cd%
popd

where python >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo Python is required. Install it from https://www.python.org/downloads/
    echo and make sure "Add python.exe to PATH" is ticked.
    exit /b 1
)

python -c "import aqt" >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo Installing aqtinstall...
    python -m pip install --quiet aqtinstall
    if !ERRORLEVEL! NEQ 0 (
        echo Failed to install aqtinstall.
        exit /b 1
    )
)

echo Installing Qt %QT_VERSION% to %QT_DIR%...
python "%SOURCE_ROOT%\scripts\setup-qt.py" --qt-version "%QT_VERSION%" --outdir "%QT_DIR%"
if !ERRORLEVEL! NEQ 0 (
    echo Qt installation failed.
    exit /b 1
)

echo.
echo Qt is installed. Build with:
echo   scripts\umbra-build.bat release
exit /b 0
