@echo off

set "config=%1"
if "%1" == "" ( set "config=debug" )
ninja -f build_%config%.ninja || exit /b %errorlevel%
copy "build\%1\ph2ed_proto.exe" ph2ed_proto.exe >nul 2>&1 || exit /b %errorlevel%
