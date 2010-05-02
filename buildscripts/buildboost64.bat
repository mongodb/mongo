@echo off

rem 64 bit version
rem address-model=64 

rem run
rem    bin\bjam --clean
rem if you switch compilers etc.

cls
echo This script builds the (64 bit) boost libs that MongoDB requires on Windows.
echo We assume boost source is in machine's \boost directory.
echo You can get boost at www.boost.org.
echo .
echo Note: you will want boost v1.42 or higher with VS2010.
echo .
echo We assume you have bjam.  To build bjam:
echo   cd tools\jam\src
echo   build.bat
echo .

cd \boost
echo bin\bjam --version
bin\bjam --version

echo .
echo .
echo .
echo About to build release libraries
pause
cls
bin\bjam --build-dir=c:\temp\boost64 address-model=64 variant=release runtime-link=static link=static --with-filesystem --with-thread --with-date_time --with-program_options --layout=versioned threading=multi toolset=msvc
echo .
echo .
echo .
echo About to try to move libs from /boost/stage/lib to /boost/lib/
pause
cls
rem bjam makes extra copies without the ver #; we kill those:
del stage\lib\*s.lib
move stage\lib\* lib\

echo .
echo .
echo .
echo About to build debug libraries
pause
cls
bin\bjam --build-dir=c:\temp\boost64 address-model=64 variant=debug --with-filesystem --with-thread --with-date_time --with-program_options --layout=versioned threading=multi toolset=msvc

echo .
echo .
echo .
echo About to try to move libs from /boost/stage/lib to /boost/lib/
pause
cls
rem bjam makes extra copies without the ver #; we kill those:
del stage\lib\*-gd.lib
move stage\lib\* lib\

echo Done - try running "dir \boost\lib\"
