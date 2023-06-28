pushd ph2ed_proto
call "./build.bat" release
popd
call tar -a -c -f ph2ed_proto.zip ph2ed_proto/ph2ed_proto.exe
