@echo off
rem Build CppWinC from any machine: locate Visual Studio (stable OR
rem pre-release, registered OR flagged incomplete) via vswhere with a
rem direct filesystem fallback; the Installer dir comes from
rem %ProgramFiles(x86)% so no drive letter is hardcoded.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_PATH="

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -prerelease -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
    if not defined VS_PATH (
        for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -prerelease -property installationPath`) do set "VS_PATH=%%i"
    )
)

rem Filesystem fallback: vswhere cannot see installs flagged incomplete or
rem unregistered Build Tools; probe the standard VS layout directly
rem ("<root>\<major|year>\<edition>" — two levels below the VS root).
if not defined VS_PATH (
    for %%P in ("%ProgramFiles%\Microsoft Visual Studio" "%ProgramFiles(x86)%\Microsoft Visual Studio") do (
        if not defined VS_PATH (
            for /d %%V in ("%%~P\*") do (
                if not defined VS_PATH (
                    for /d %%E in ("%%~V\*") do (
                        if exist "%%~E\VC\Tools\MSVC" (
                            for /d %%T in ("%%~E\VC\Tools\MSVC\*") do (
                                if exist "%%~T\bin\Hostx64\x64\cl.exe" (
                                    set "VS_PATH=%%~E"
                                )
                            )
                        )
                    )
                )
            )
        )
    )
)

if not defined VS_PATH (
    echo ERROR: no Visual Studio with VC tools found ^(vswhere + filesystem scan^).
    exit /b 1
)

echo Using Visual Studio: %VS_PATH%
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d "%~dp0"

if not exist "build" mkdir "build"
set "CLF=/nologo /std:c++latest /EHa /MD /O2 /DNDEBUG /GL /utf-8 /bigobj /ifcSearchDir build /ifcOutput build /Fobuild/"

echo [1/11] winmd_reader...
cl %CLF% /c winmd_reader.ixx > build/winmd_reader.log 2>&1
if errorlevel 1 (echo FAIL: winmd_reader & exit /b 1)
echo   OK

echo [2/11] async_data...
cl %CLF% /c async_data.ixx > build/async_data.log 2>&1
if errorlevel 1 (echo FAIL: async_data & exit /b 1)
echo   OK

echo [3/11] component_data...
cl %CLF% /c component_data.ixx > build/component_data.log 2>&1
if errorlevel 1 (echo FAIL: component_data & exit /b 1)
echo   OK

echo [4/11] win32_data...
cl %CLF% /c win32_data.ixx > build/win32_data.log 2>&1
if errorlevel 1 (echo FAIL: win32_data & exit /b 1)
echo   OK

echo [5/11] wdk_data...
cl %CLF% /c wdk_data.ixx > build/wdk_data.log 2>&1
if errorlevel 1 (echo FAIL: wdk_data & exit /b 1)
echo   OK

echo [6/11] winuic_helper...
cl %CLF% /c winuic_helper.ixx > build/winuic_helper.log 2>&1
if errorlevel 1 (echo FAIL: winuic_helper & exit /b 1)
echo   OK

echo [7/11] winuic_data...
cl %CLF% /c winuic_data.ixx > build/winuic_data.log 2>&1
if errorlevel 1 (echo FAIL: winuic_data & exit /b 1)
echo   OK

echo [8/11] strings_data...
cl %CLF% /c strings_data.ixx > build/strings_data.log 2>&1
if errorlevel 1 (echo FAIL: strings_data & exit /b 1)
echo   OK

echo [9/11] cppwinc...
cl %CLF% /c cppwinc.ixx > build/cppwinc.log 2>&1
if errorlevel 1 (echo FAIL: cppwinc & exit /b 1)
echo   OK

echo [10/11] main...
cl %CLF% /c main.cpp > build/main.log 2>&1
if errorlevel 1 (echo FAIL: main & exit /b 1)
echo   OK

echo [11/11] link...
link /nologo /out:cppwinc.exe /RELEASE /OPT:REF /OPT:ICF /LTCG /MANIFEST:EMBED /MANIFESTINPUT:app.manifest "build\main.obj" "build\cppwinc.obj" "build\winmd_reader.obj" "build\async_data.obj" "build\strings_data.obj" "build\component_data.obj" "build\win32_data.obj" "build\wdk_data.obj" "build\winuic_helper.obj" "build\winuic_data.obj" advapi32.lib ole32.lib > build/link.log 2>&1
if errorlevel 1 (echo FAIL: link & type build\link.log & exit /b 1)

echo === cppwinc.exe built successfully ===
exit /b 0
