@echo off

pushd "%~dp0"
if "%LIB%" == "" ( if exist "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat" call "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build/vcvars64.bat" >nul )
if "%LIB%" == "" ( if exist "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat" call "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat" >nul )
set "config=%1"
if "%1" == "" ( set "config=debug" )
ninja -f build_%config%.ninja || goto end
copy "build\%config%\ph2ed_proto.exe" ph2ed_proto.exe || goto end
:end
popd
echo Exiting with code %errorlevel%
exit /b %errorlevel%
