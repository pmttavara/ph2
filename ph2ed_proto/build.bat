@echo off
set "vctoolsdir=%programfiles(x86)%\Microsoft Visual Studio 14.0\VC\"
set "winkits10include=%programfiles(x86)%\Windows Kits\10\include\10.0.10240.0\"
set "winkits10lib=%programfiles(x86)%\Windows Kits\10\lib\10.0.10240.0\"
set "winkits8dir=%programfiles(x86)%\Windows Kits\8.1\"
set "INCLUDE=%vctoolsdir%\include;%winkits10include%\ucrt;%winkits8dir%\include\shared;%winkits8dir%\include\um;"
set "LIB=%vctoolsdir%\lib\amd64;%winkits8dir%\lib\winv6.3\um\x64;%winkits10lib%\ucrt\x64;"
set "PATH=%vctoolsdir%\bin\amd64;%PATH%"
sokol-shdc -i main.glsl -o main.glsl.h --slang hlsl5:glsl330 || exit /b 1
cl -Od -Z7 -nologo -W4 -WX -wd4189 -EHsc- main.cpp -c -Fomain.obj || exit /b 1
cl -Od -Z7 -nologo -W4 -WX -wd4189 -EHsc- imgui_full.cpp -c -Foimgui_full.obj || exit /b 1
link -debug -nologo main.obj imgui_full.obj /out:ph2ed_proto.exe || exit /b 1
