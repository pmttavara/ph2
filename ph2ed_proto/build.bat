@echo off
setlocal enabledelayedexpansion

sokol-shdc -i shaders.glsl -o shaders.glsl.h --slang hlsl5 --bytecode
if !errorlevel! neq 0 goto end

REM Debug modes:
if "1"=="1" (

cl       -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4457 -wd4800                                 -EHa- main.cpp -c -Fomain.obj /RTCu /fsanitize=address
if !errorlevel! neq 0 goto end
cl       -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4457 -wd4800 -wd4334 -wd4244 -wd4267 -wd4706 -EHa- libs.cpp -c -Folibs.obj /RTCu /fsanitize=address
if !errorlevel! neq 0 goto end
REM clang-cl -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4457 -wd4800                                 -Wno-missing-braces -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -Wno-tautological-constant-out-of-range-compare -Wno-missing-field-initializers -EHa- -fsanitize=address -fsanitize=undefined main.cpp -c -Fomain.obj
if !errorlevel! neq 0 goto end
REM clang-cl -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4457 -wd4800 -wd4334 -wd4244 -wd4267 -wd4706 -Wno-missing-braces -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -Wno-tautological-constant-out-of-range-compare -Wno-missing-field-initializers -EHa- -fsanitize=address -fsanitize=undefined libs.cpp -c -Folibs.obj
if !errorlevel! neq 0 goto end

link     -noimplib -noexp -debug -incremental:no -nologo main.obj libs.obj /out:ph2ed_proto.exe
if !errorlevel! neq 0 goto end
REM lld-link                  -debug -incremental:no -nologo main.obj libs.obj /out:ph2ed_proto.exe
if !errorlevel! neq 0 goto end
REM clang -fuse-ld=lld -g -gfull main.obj libs.obj -o ph2ed_proto.exe -fsanitize=address -fsanitize=undefined
if !errorlevel! neq 0 goto end

) else (
REM Release mode:

cl                    -DNDEBUG -O2 -Z7 -nologo -EHa- main.cpp -Feph2ed_proto.exe -link -noimplib -noexp
if !errorlevel! neq 0 goto end
REM clang-cl -fuse-ld=lld -DNDEBUG -O2 -Z7 -nologo -EHa- main.cpp -Feph2ed_proto.exe
if !errorlevel! neq 0 goto end

)

exit /b 0
:end
exit /b 1

