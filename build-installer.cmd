@ECHO OFF
SETLOCAL EnableDelayedExpansion
CD /D %~dp0

rem If launched from anything other than cmd.exe, will have "%WINDIR%\system32\cmd.exe" in the command line
ECHO.%CMDCMDLINE% | FINDSTR /C:"%COMSPEC% /c" >NUL
IF ERRORLEVEL 1 GOTO NONINTERACTIVE
rem Preserve this as it seems to be corrupted below otherwise?!
SET CMDLINE=%CMDCMDLINE%
rem If launched from anything other than cmd.exe, last character of command line will always be a double quote
IF NOT ^!CMDCMDLINE:~-1!==^" GOTO NONINTERACTIVE
rem If run from Explorer, last-but-one character of command line will be a space
IF NOT "!CMDLINE:~-2,1!"==" " GOTO NONINTERACTIVE
SET INTERACTIVE=1
:NONINTERACTIVE

SET WIX_PATH=
SET FIND_CANDLE=
FOR %%p IN (candle.exe) DO SET "FIND_CANDLE=%%~$PATH:p"
IF NOT DEFINED FIND_CANDLE GOTO LOCATE
SET WIX_PATH=%FIND_CANDLE:~0,-10%
ECHO WiX Toolset located on path: %WIX_PATH%
GOTO BUILD

:LOCATE
ECHO Searching for WiX Toolset...
FOR /F "usebackq tokens=*" %%f IN (`DIR /B /ON "%ProgramFiles(x86)%\WiX Toolset*"`) DO IF EXIST "%ProgramFiles(x86)%\%%f\bin\candle.exe" SET "WIX_PATH=%ProgramFiles(x86)%\%%f\bin\"
IF "%WIX_PATH%"=="" ECHO Cannot find WiX Toolset & GOTO ERROR
ECHO WiX Toolset found at: %WIX_PATH%

:BUILD
ECHO Building...
"%WIX_PATH%candle.exe" -ext WixUtilExtension brightly.wxs
IF ERRORLEVEL 1 GOTO ERROR

ECHO Building...
"%WIX_PATH%light.exe" -sice:ICE91 -ext WixUtilExtension -ext WixUIExtension brightly.wixobj
IF ERRORLEVEL 1 GOTO ERROR

ECHO Done.
IF DEFINED INTERACTIVE COLOR 2F & PAUSE & COLOR
GOTO :EOF

:ERROR
ECHO ERROR: An error occured.
IF DEFINED INTERACTIVE COLOR 4F & PAUSE & COLOR
EXIT /B 1
GOTO :EOF
