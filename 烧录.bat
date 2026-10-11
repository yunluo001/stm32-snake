@echo off
echo ================================================================
echo   BUILD + FLASH  (no debugger needed)
echo   ------------------------------------------------------------
echo   Before running: press Shift+F5 in VS Code to stop debugging,
echo   otherwise the ST-Link is busy and flashing will fail.
echo ================================================================
echo.

set "PATH=C:\Users\77601\AppData\Local\stm32cube\bundles\cmake\4.3.1+st.1\bin;C:\Users\77601\AppData\Local\stm32cube\bundles\ninja\1.13.2+st.1\bin;C:\Users\77601\AppData\Local\stm32cube\bundles\gnu-tools-for-stm32\14.3.1+st.2\bin;%PATH%"
set "CLI=C:\Users\77601\AppData\Local\stm32cube\bundles\programmer\2.23.0\bin\STM32_Programmer_CLI.exe"
set "ELF=D:\STM32\TEXT\1083\build\Debug\1083.elf"

echo [1/2] BUILD...
cmake --build "D:\STM32\TEXT\1083\build\Debug"
if errorlevel 1 goto buildfail

echo.
echo [2/2] FLASH...
rem  1 MHz 的 SWD 更稳；-e all 先整片擦除再写，能躲开 "failed to download Sector" 这类毛病
"%CLI%" -c port=SWD freq=100 mode=UR -e all -w "%ELF%" -v -rst
if errorlevel 1 goto flashfail

echo.
echo ================================================================
echo   DONE - the program is already running.
echo   (Look at the board: the on-board LED should be blinking.)
echo ================================================================
goto end

:buildfail
echo.
echo *** BUILD FAILED - check the errors above ***
goto end

:flashfail
echo.
echo *** FLASH FAILED ***
echo   Check: USB cable, the 4 SWD wires, and that the board has power.
echo   Also make sure the VS Code debug session is stopped (Shift+F5).

:end
echo.
pause
