@echo off
setlocal
set R=..\go\src\github.com\google\flatbuffers\src
set GRPC=..\go\src\github.com\google\flatbuffers\grpc
if exist flatc\flatc.exe goto have_flatc
mkdir flatc
cd flatc
cl /Fe: flatc.exe /MP /EHsc -I ..\go\src\github.com\google\flatbuffers\include -I %GRPC% %R%\flatc.cpp %R%\idl_gen_cpp.cpp %R%\idl_gen_fbs.cpp %R%\idl_gen_general.cpp %R%\idl_gen_go.cpp %R%\idl_gen_grpc.cpp %R%\idl_gen_js.cpp %R%\idl_gen_php.cpp %R%\idl_gen_python.cpp %R%\idl_gen_text.cpp %R%\idl_parser.cpp %R%\reflection.cpp %R%\util.cpp %GRPC%\src\compiler\cpp_generator.cc
cd..
:have_flatc
flatc\flatc --cpp -o cpp http-bridge.fbs
flatc\flatc --go -o go\src http-bridge.fbs
