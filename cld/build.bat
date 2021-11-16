@echo off
set "vctoolsdir=%programfiles(x86)%\Microsoft Visual Studio 14.0\VC\"
set "winkits10include=%programfiles(x86)%\Windows Kits\10\include\10.0.10240.0\"
set "winkits10lib=%programfiles(x86)%\Windows Kits\10\lib\10.0.10240.0\"
set "winkits8dir=%programfiles(x86)%\Windows Kits\8.1\"
set "INCLUDE=%vctoolsdir%\include;%winkits10include%\ucrt;%winkits8dir%\include\shared;%winkits8dir%\include\um;"
set "LIB=%vctoolsdir%\lib\amd64;%winkits8dir%\lib\winv6.3\um\x64;%winkits10lib%\ucrt\x64;"
set "PATH=%vctoolsdir%\bin\amd64;%PATH%"
clang -O0 -Werror -Weverything -std=c++20 ph2_cld_test_cpp.cpp -c -o ph2_cld_test_cpp_clang.obj || exit /b 1
clang -O0 -Werror -Weverything -std=c89   ph2_cld.c ph2_cld_test.c -o ph2_cld_test_clang.exe || exit /b 1
cl -Od -Z7 -nologo -Wall -WX -wd4820 -wd4710 -wd4514 -EHsc- ph2_cld_test_cpp.cpp -c -Foph2_cld_test_cpp.obj || exit /b 1
cl -Od -Z7 -nologo -Wall -WX -wd4820 -wd4710 -wd4514 -EHsc- ph2_cld.c ph2_cld_test.c -Feph2_cld_test.exe || exit /b 1
