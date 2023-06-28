::
:: This is the Continuous Integration script, don't run this on your PC!
::

call "ph2ed_proto/build.bat" release || exit /b %errorlevel%
call powershell Compress-Archive -Force -Path .\ph2ed_proto\ph2ed_proto.exe -DestinationPath ph2ed_proto.zip
if %errorlevel% neq 0 exit /b %errorlevel%
