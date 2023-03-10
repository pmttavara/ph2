@echo off

set VSWHERE="C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=1* delims=: " %%i in (`%VSWHERE% -latest -requires Microsoft.Component.MSBuild`) do (
  if /i "%%i"=="installationPath" set VS=%%j
)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" amd64 >nul

sokol-shdc -i shaders.glsl -o shaders.glsl.h --slang hlsl5 --bytecode || exit /b 1

REM Debug modes:
if "1"=="1" (

cl       -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4457 -wd4800                                 -EHa- main.cpp -c -Fomain.obj /RTCu /fsanitize=address || exit /b 1
cl       -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4457 -wd4800 -wd4334 -wd4244 -wd4267 -wd4706 -EHa- libs.cpp -c -Folibs.obj /RTCu /fsanitize=address || exit /b 1
REM clang-cl -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4457 -wd4800                                 -Wno-missing-braces -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -Wno-tautological-constant-out-of-range-compare -Wno-missing-field-initializers -EHa- -fsanitize=address -fsanitize=undefined main.cpp -c -Fomain.obj || exit /b 1
REM clang-cl -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4457 -wd4800 -wd4334 -wd4244 -wd4267 -wd4706 -Wno-missing-braces -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -Wno-tautological-constant-out-of-range-compare -Wno-missing-field-initializers -EHa- -fsanitize=address -fsanitize=undefined libs.cpp -c -Folibs.obj || exit /b 1

link     -noimplib -noexp -debug -incremental:no -nologo main.obj libs.obj /out:ph2ed_proto.exe || exit /b 1
REM lld-link                  -debug -incremental:no -nologo main.obj libs.obj /out:ph2ed_proto.exe || exit /b 1
REM clang -fuse-ld=lld -g -gfull main.obj libs.obj -o ph2ed_proto.exe -fsanitize=address -fsanitize=undefined || exit /b 1

) else (
REM Release mode:

REM cl                    -DNDEBUG -O2 -Z7 -nologo -EHa- main.cpp -Feph2ed_proto.exe -link -noimplib -noexp || exit /b 1
clang-cl -fuse-ld=lld -DNDEBUG -O2 -Z7 -nologo -EHa- main.cpp -Feph2ed_proto.exe || exit /b 1

)
