::
:: This is the Continuous Integration script, don't run this on your PC!
::

call "ph2ed_proto/build.bat" release
if %errorlevel% neq 0 exit /b 1
call powershell Compress-Archive -Force -Path .\ph2ed_proto\ph2ed_proto.exe,.\ph2ed_proto\ph2ed_proto.pdb -DestinationPath ph2ed_proto.zip
if %errorlevel% neq 0 exit /b 1
