REM Starts OpenOCD - designed to be called from GDB, itself started from Visual Studio launch.vs.json
REM If already running it will fail and exit, leaving the existing one to handle GDB requests
REM Template allows for multiple target interfaces and boards - add new ones as required
REM DAV 26FEB19
setlocal
set cmd=C:/idf/.espressif/tools/openocd-esp32/v0.12.0-esp32-20240821/openocd-esp32/bin/openocd.exe -s C:/idf/.espressif/tools/openocd-esp32/v0.12.0-esp32-20240821/openocd-esp32/share/openocd/scripts/

if %1.==. goto ESP32_JLINK
call :%1
if %errorlevel% equ 1 goto :lblerror
goto end

REM Start OpenOCD
:start
start %cmd% %iface%
goto :eof

:end
endlocal
exit

:lblerror
mshta javascript:alert("Error: Unknown label %1");close();
goto end

REM Add additional interface/board combinations here, and add the label to the launch.vs.json file

:ESP32_JLINK
set iface=-f interface/esp_usb_bridge.cfg -f target/esp32s3.cfg -c "adapter_khz 3000"
goto start
