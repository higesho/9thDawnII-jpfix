@echo off
rem build jpfix proxy DLL (x86)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /LD /O2 /MT /arch:IA32 /EHsc /utf-8 /W3 src\jpfix.cpp /Fobuild\ /Fdbuild\ /FAs /Fabuild\ /link /OUT:build\version.dll /IMPLIB:build\jpfix.lib user32.lib opengl32.lib
