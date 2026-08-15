@echo off

pushd "%~dp0"
if not "%LIB%" == "" goto have_msvc
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" set "vswhere=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "vcvars="
if exist "%vswhere%" for /f "usebackq delims=" %%i in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vcvars=%%i\VC\Auxiliary\Build\vcvars64.bat"
if exist "%vcvars%" call "%vcvars%" >nul
if "%LIB%" == "" ( if exist "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat" call "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat" >nul )
if "%LIB%" == "" ( if exist "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat" call "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat" >nul )
if "%LIB%" == "" ( echo build.bat: no MSVC environment - could not find vcvars64.bat & popd & exit /b 1 )
:have_msvc
set "config=%1"
if "%1" == "" ( set "config=release" )
echo git_hash = $> build.ninja
git rev-parse --short HEAD >> build.ninja
type build_%config%.ninja >> build.ninja
ninja || goto end
copy "build\%config%\ph2ed_proto.exe" ph2ed_proto.exe >nul || goto end
copy "build\%config%\ph2ed_proto.pdb" ph2ed_proto.pdb >nul || goto end
:end
popd
exit /b %errorlevel%
