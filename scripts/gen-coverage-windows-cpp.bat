@echo off
setlocal
set root=%cd%

rem A non-optimized build gives better line coverage information
set build=win64-msvc-debug-default

del cov1.out
del go\cov2.out

rem Run C++ only tests
OpenCppCoverage.exe --export_type binary:cov1.out --modules %root% --sources %root%\cpp -- %root%\t2-output\%build%\unit-test.exe

rem Run Go-based test suite
cd go
call env.bat
rem Get the Go testing infrastructure to build the C++ test-backend.exe
go test httpbridge -run Echo
OpenCppCoverage.exe --export_type binary:cov2.out --modules %root% --sources %root%\cpp --cover_children -- c:\go\bin\go.exe test httpbridge -skip_build
cd %root%

rem Merge results
rmdir /s /q coverage-report
OpenCppCoverage.exe --input_coverage cov1.out --input_coverage go\cov2.out --export_type html:coverage-report

del cov1.out
del go\cov2.out
