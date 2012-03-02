@echo off
rem Copyright Beman Dawes, 2010
rem Distributed under the Boost Software License, Version 1.0.
rem See www.boost.org/LICENSE_1_0.txt

copy /y ..\tut1.cpp >nul
copy /y ..\tut2.cpp >nul
copy /y ..\tut3.cpp >nul
copy /y ..\tut4.cpp >nul
copy /y ..\tut5.cpp >nul
copy /y ..\path_info.cpp >nul
del *.exe 2>nul
del *.pdb 2>nul
