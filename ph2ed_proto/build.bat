@echo off

pushd "%~dp0"
if "%LIB%" == "" ( if exist "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat" call "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat" >nul )
if "%LIB%" == "" ( if exist "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat" call "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat" >nul )
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
