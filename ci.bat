call "ph2ed_proto/build.bat" debug || exit /b %errorlevel%
call powershell Compress-Archive -Force -Path .\ph2ed_proto\ph2ed_proto.exe -DestinationPath ph2ed_proto.zip
if %errorlevel% neq 0 exit /b %errorlevel%
