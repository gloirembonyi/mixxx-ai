@ECHO OFF
REM Builds Mixxx on Windows with the Visual Studio 2022 Build Tools.
REM
REM Usage:
REM   build_mixxx.bat          configure (first time) and build
REM   build_mixxx.bat test     build and run the unit tests
REM   build_mixxx.bat run      build and start Mixxx
REM   build_mixxx.bat dev      build and start Mixxx in developer mode
REM                            (Developer menu, control picker, debug log)
REM
REM Close Mixxx before building, Windows locks the running mixxx.exe.
REM The prebuilt dependencies are downloaded to buildenv\ on the first run.
SETLOCAL

SET "MIXXX_ROOT=%~dp0"
SET "BUILD_DIR=%MIXXX_ROOT%build\portable"

REM Locate Visual Studio (Build Tools or any edition) with the C++ workload
SET "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
IF EXIST "%VSWHERE%" (
    FOR /F "usebackq tokens=*" %%i IN (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) DO SET "VS_PATH=%%i"
)
IF NOT DEFINED VS_PATH (
    IF EXIST "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        SET "VS_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
    )
)
IF NOT DEFINED VS_PATH (
    ECHO ERROR: Visual Studio 2022 with the C++ build tools was not found.
    EXIT /B 1
)
CALL "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >NUL || EXIT /B 1

REM Download/unpack the dependencies and set MIXXX_VCPKG_ROOT.
REM GITHUB_ENV makes the script skip regenerating CMakeSettings.json on
REM every build (it only appends the variables to that file).
SET BUILDENV_RELEASE=1
SET "GITHUB_ENV=%TEMP%\mixxx_buildenv_vars.txt"
CALL "%MIXXX_ROOT%tools\windows_buildenv.bat" setup || EXIT /B 1
DEL /Q "%GITHUB_ENV%" 2>NUL
SET GITHUB_ENV=

IF NOT EXIST "%BUILD_DIR%\build.ninja" (
    cmake -S "%MIXXX_ROOT%." -B "%BUILD_DIR%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
        -DCMAKE_TOOLCHAIN_FILE="%MIXXX_VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
        -DVCPKG_TARGET_TRIPLET=x64-windows-release ^
        -DMIXXX_VCPKG_ROOT="%MIXXX_VCPKG_ROOT%" ^
        -DOPTIMIZE=portable ^
        -DDEBUG_ASSERTIONS_FATAL=OFF ^
        -DQT6=ON -DSTATIC_DEPS=OFF ^
        -DBATTERY=ON -DBROADCAST=ON -DBULK=ON -DHID=ON -DHSS1394=ON ^
        -DLILV=ON -DMAD=ON -DMEDIAFOUNDATION=ON -DMODPLUG=ON -DOPUS=ON ^
        -DQTKEYCHAIN=ON -DVINYLCONTROL=ON -DWAVPACK=ON ^
        -DFFMPEG=OFF -DKEYFINDER=OFF -DLOCALECOMPARE=ON || EXIT /B 1
)

cmake --build "%BUILD_DIR%" --parallel || EXIT /B 1

IF /I "%~1"=="test" (
    PUSHD "%BUILD_DIR%"
    mixxx-test.exe
    SET TEST_RESULT=%ERRORLEVEL%
    POPD
    EXIT /B %TEST_RESULT%
)

REM Mixxx needs forward slashes in the resource path
SET "RES_PATH=%MIXXX_ROOT:\=/%res/"
IF /I "%~1"=="run" (
    START "" "%BUILD_DIR%\mixxx.exe" --resourcePath "%RES_PATH%"
)
IF /I "%~1"=="dev" (
    START "" "%BUILD_DIR%\mixxx.exe" --resourcePath "%RES_PATH%" --developer --log-level debug
)

ENDLOCAL
