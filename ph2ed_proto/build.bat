@echo off

sokol-shdc -i shaders.glsl -o shaders.glsl.h --slang hlsl5 --bytecode || exit /b 1

:: Debug modes:

cl -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4457 -wd4800 -EHa- main.cpp -c -Fomain.obj || exit /b 1
cl -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4457 -wd4800 -EHa- libs.cpp -c -Folibs.obj || exit /b 1
:: clang-cl -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4800 -Wno-missing-braces -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -Wno-tautological-constant-out-of-range-compare -EHa- -fsanitize=address -fsanitize=undefined main.cpp -c -Fomain.obj || exit /b 1
:: clang-cl -Od -Z7 -nologo -W4 -WX -wd4189 -wd4456 -wd4800 -Wno-missing-braces -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -Wno-tautological-constant-out-of-range-compare -EHa- -fsanitize=address -fsanitize=undefined libs.cpp -c -Folibs.obj || exit /b 1

:: link -debug -incremental:no -nologo main.obj libs.obj /out:ph2ed_proto.exe legacy_stdio_definitions.lib comdlg32.lib || exit /b 1
lld-link -debug -incremental:no -nologo main.obj libs.obj /out:ph2ed_proto.exe legacy_stdio_definitions.lib comdlg32.lib || exit /b 1
:: clang -fuse-ld=lld -g -gfull main.obj libs.obj -o ph2ed_proto.exe -fsanitize=address -fsanitize=undefined -llegacy_stdio_definitions -lcomdlg32 || exit /b 1

:: Release mode:

:: clang-cl -fuse-ld=lld -DNDEBUG -O2 -Z7 -nologo -EHa- main.cpp -c -Fomain.obj || exit /b 1
:: clang -fuse-ld=lld -g -gfull main.obj -o ph2ed_proto.exe -llegacy_stdio_definitions -lcomdlg32 || exit /b 1
