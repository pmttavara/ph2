@echo off
cl -Od -Z7 -nologo -Wall -WX -wd4820 -wd4710 -wd4514 -wd4514 -wd5045 -EHsc- ph2_cld_test_cpp.cpp -Feph2_cld_test_cpp.exe -link -noimplib -noexp -incremental:no || exit /b 1
cl -Od -Z7 -nologo -Wall -WX -wd4820 -wd4710 -wd4711 -wd4514 -wd5045 -EHsc- ph2_cld.c ph2_cld_test.c -Feph2_cld_test.exe    -link -noimplib -noexp -incremental:no || exit /b 1
