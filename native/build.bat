@echo off
rem Builds kh2crashdump.dll (and a test lua.exe) against Lua 5.4.8 sources.
rem usage: build.bat <folder with lua.h of Lua 5.4.8>
rem Needs MSVC x64 (run from a "x64 Native Tools" prompt, or this script calls vcvars64).
setlocal
set LUASRC=%~1
if "%LUASRC%"=="" (echo usage: build.bat ^<lua-5.4.8 source folder^> & exit /b 1)
if not exist "%LUASRC%\lua.h" (echo lua.h not found in %LUASRC% & exit /b 1)
findstr /c:"#define LUA_VERSION_RELEASE	\"8\"" "%LUASRC%\lua.h" >nul || (echo need Lua 5.4.8 exactly ^(LuaBackend embeds 5.4.8^) & exit /b 1)

where cl >nul 2>nul || call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul || (echo cl.exe not found & exit /b 1)

set OUT=%~dp0build
if not exist "%OUT%" mkdir "%OUT%"
set CORE=lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib
set LIBS=lbaselib lcorolib ldblib liolib lmathlib loadlib loslib lstrlib ltablib lutf8lib linit

set SRCS=
for %%f in (%CORE%) do call set SRCS=%%SRCS%% "%LUASRC%\%%f.c"

rem /MT: static CRT like LuaBackend.dll. No LUA_BUILD_AS_DLL: the core stays private to the module.
cl /nologo /O2 /MT /W3 /LD /I"%LUASRC%" /D_CRT_SECURE_NO_WARNINGS "%~dp0kh2crashdump.c" %SRCS% ^
   /Fo"%OUT%\\" /Fe"%OUT%\kh2crashdump.dll" || exit /b 1

set TSRCS=%SRCS%
for %%f in (%LIBS%) do call set TSRCS=%%TSRCS%% "%LUASRC%\%%f.c"
rem test interpreter with its OWN copy of the core, mirroring LuaBackend
cl /nologo /O2 /MT /W3 /I"%LUASRC%" /D_CRT_SECURE_NO_WARNINGS "%LUASRC%\lua.c" %TSRCS% ^
   /Fo"%OUT%\\" /Fe"%OUT%\lua54_test.exe" || exit /b 1

del "%OUT%\*.obj" "%OUT%\*.exp" 2>nul
echo built %OUT%\kh2crashdump.dll
