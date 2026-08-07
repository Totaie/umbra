@echo off

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set RULE_NAME=Umbra Host
set PROGRAM_BIN=%ROOT_DIR%\sunshine.exe

rem NB: name= and program= must be quoted. This rule used to be called "Apollo",
rem which has no space, so an unquoted name= happened to work; "Umbra Host" made
rem netsh read "Host" as a stray argument and the rule was silently never created.
rem The install still reported success, and the host was simply unreachable from
rem anywhere but the machine it runs on.
netsh advfirewall firewall add rule name="%RULE_NAME%" dir=in action=allow protocol=tcp program="%PROGRAM_BIN%" enable=yes
netsh advfirewall firewall add rule name="%RULE_NAME%" dir=in action=allow protocol=udp program="%PROGRAM_BIN%" enable=yes
