@echo off
setlocal enabledelayedexpansion

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set SERVICE_NAME=UmbraService
set "SERVICE_BIN=%ROOT_DIR%\tools\sunshinesvc.exe"
set "SERVICE_CONFIG_DIR=%LOCALAPPDATA%\SudoMaker\Apollo"
set "SERVICE_CONFIG_FILE=%SERVICE_CONFIG_DIR%\service_start_type.txt"

rem Start with Windows, before anyone logs in.
rem
rem This was demand, which meant the service only ran because the installer started it
rem and never came back after a reboot - so the machine was unreachable until somebody
rem walked over and signed in, which is the one thing a remote desktop has to work
rem without. The service runs as LocalSystem and launches the host into the console
rem session, so it is up and answering at the login screen.
rem
rem A start type the user chose themselves is still honoured; see the saved value below.
set SERVICE_START_TYPE=auto

rem Remove the legacy SunshineSvc service
net stop sunshinesvc
sc delete sunshinesvc

rem Remove ApolloService too. That's what this service was called before the Umbra
rem rename, and leaving it registered means two services fighting over port 47989.
net stop ApolloService
sc delete ApolloService

rem Check if UmbraService already exists
sc qc %SERVICE_NAME% > nul 2>&1
if %ERRORLEVEL%==0 (
    rem Stop the existing service if running
    net stop %SERVICE_NAME%

    rem Reconfigure the existing service
    set SC_CMD=config
) else (
    rem Create a new service
    set SC_CMD=create
)

rem Check if we have a saved start type from previous installation
if exist "%SERVICE_CONFIG_FILE%" (
    rem Debug output file content
    type "%SERVICE_CONFIG_FILE%"

    rem Read the saved start type
    for /f "usebackq delims=" %%a in ("%SERVICE_CONFIG_FILE%") do (
        set "SAVED_START_TYPE=%%a"
    )

    echo Raw saved start type: [!SAVED_START_TYPE!]

    rem Check start type
    if "!SAVED_START_TYPE!"=="2-delayed" (
        set SERVICE_START_TYPE=delayed-auto
    ) else if "!SAVED_START_TYPE!"=="2" (
        set SERVICE_START_TYPE=auto
    ) else if "!SAVED_START_TYPE!"=="3" (
        set SERVICE_START_TYPE=demand
    ) else if "!SAVED_START_TYPE!"=="4" (
        set SERVICE_START_TYPE=disabled
    )

    del "%SERVICE_CONFIG_FILE%"
)

echo Setting service start type set to: [!SERVICE_START_TYPE!]

rem Run the sc command to create/reconfigure the service
sc %SC_CMD% %SERVICE_NAME% binPath= "\"%SERVICE_BIN%\"" start= %SERVICE_START_TYPE% DisplayName= "Umbra Host Service"

rem Set the description of the service
sc description %SERVICE_NAME% "Umbra Host lets other devices connect to this PC with Umbra."

rem Start the new service
net start %SERVICE_NAME%
