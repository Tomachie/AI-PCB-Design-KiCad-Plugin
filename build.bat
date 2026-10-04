@echo off
rem Builds out\tweb.exe with Visual Studio 2022 (MSVC, C++17).
rem No other dependencies: KiCad's own nng.dll and zlib1.dll are loaded at run time.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo VCVARS FAILED & exit /b 1)
cd /d "%~dp0"
if not exist out mkdir out
rc /nologo /fo out\tweb.res tweb.rc
if errorlevel 1 (echo RC FAILED & exit /b 1)
cl /nologo /EHa /O2 /W3 /std:c++17 /Foout\ tweb.cpp string_table.cpp out\tweb.res ^
   /Fe:out\tweb.exe ^
   /link user32.lib shell32.lib winhttp.lib gdi32.lib
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo built out\tweb.exe
exit /b 0
