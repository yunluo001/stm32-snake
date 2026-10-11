@echo off
echo ================================================================
echo   FLASH WHILE HOLDING RESET  (for when SWD can't connect)
echo ================================================================
echo.
echo   The RST wire is not connected to the ST-Link, so the debugger
echo   cannot hold the chip in reset by itself. Do it by hand:
echo.
echo     STEP 1:  PRESS AND HOLD the RESET button on the board
echo     STEP 2:  press any key here to start flashing
echo     STEP 3:  keep holding for about 3 seconds, then release
echo.
pause

set "CLI=C:\Users\77601\AppData\Local\stm32cube\bundles\programmer\2.23.0\bin\STM32_Programmer_CLI.exe"
set "ELF=D:\STM32\TEXT\1083\build\Debug\1083.elf"

echo.
echo FLASHING (hold RESET now, release in ~3s)...
"%CLI%" -c port=SWD freq=100 mode=UR -w "%ELF%" -v -rst

echo.
echo ================================================================
echo   If you see "Download verified successfully" + "MCU Reset",
echo   it worked.
echo ================================================================
pause
