@echo off
cl -Od -Z7 -nologo -W4 -WX -EHsc- cld_proto.cpp -link user32.lib -noimplib -noexp -incremental:no
