@echo off

rem "Apollo" is what this rule was called before the Umbra rename. Remove both, or
rem uninstalling leaves an inbound allow rule pointing at a path that no longer
rem exists - and a future install at the same path inherits it silently.
netsh advfirewall firewall delete rule name="Umbra Host"
netsh advfirewall firewall delete rule name="Apollo"
