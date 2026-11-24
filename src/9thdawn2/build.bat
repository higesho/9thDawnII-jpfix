@echo off
rem build jpfix proxy DLL (x86)
rem   build.bat       -> release build: build\version.dll
rem   build.bat test  -> test build (JPFIX_TEST): build\test\version.dll  (never distribute)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
cd /d "%~dp0"
set O=build\release
set OUT=build\version.dll
set DEF=
if "%1"=="test" (
  set O=build\test
  set OUT=build\test\version.dll
  set DEF=/DJPFIX_TEST
)
if not exist %O% mkdir %O%
rc /nologo /fo %O%\jpfix.res src\jpfix.rc || exit /b 1
cl /nologo /LD /O2 /MT /arch:IA32 /EHsc /utf-8 /W3 %DEF% src\jpfix.cpp %O%\jpfix.res /Fo%O%\ /Fd%O%\ /FAs /Fa%O%\ /link /OUT:%OUT% /IMPLIB:%O%\jpfix.lib user32.lib opengl32.lib || exit /b 1
