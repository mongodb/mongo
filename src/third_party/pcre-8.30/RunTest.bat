@echo off
@rem This file must use CRLF linebreaks to function properly
@rem and requires both pcretest and pcregrep
@rem  This file was originally contributed by Ralf Junker, and touched up by
@rem  Daniel Richard G. Tests 10-12 added by Philip H.
@rem  Philip H also changed test 3 to use "wintest" files.
@rem
@rem  Updated by Tom Fortmann to support explicit test numbers on the command line.
@rem  Added argument validation and added error reporting.
@rem
@rem  MS Windows batch file to run pcretest on testfiles with the correct
@rem  options.
@rem
@rem Sheri Pierce added logic to skip feature dependent tests
@rem tests 4 5 9 15 and 18 require utf support
@rem tests 6 7 10 16 and 19 require ucp support
@rem 11 requires ucp and link size 2
@rem 12 requires presense of jit support
@rem 13 requires absence of jit support
@rem Sheri P also added override tests for study and jit testing
@rem Zoltan Herczeg added libpcre16 support

setlocal enabledelayedexpansion
if [%srcdir%]==[] (
if exist testdata\ set srcdir=.)
if [%srcdir%]==[] (
if exist ..\testdata\ set srcdir=..)
if [%srcdir%]==[] (
if exist ..\..\testdata\ set srcdir=..\..)
if NOT exist "%srcdir%\testdata\" (
Error: echo distribution testdata folder not found!
call :conferror
exit /b 1
goto :eof
)

if "%pcretest%"=="" set pcretest=.\pcretest.exe

echo source dir is %srcdir%
echo pcretest=%pcretest%

if NOT exist "%pcretest%" (
echo Error: "%pcretest%" not found!
echo.
call :conferror
exit /b 1
)

"%pcretest%" -C linksize >NUL
set link_size=%ERRORLEVEL%
"%pcretest%" -C pcre8 >NUL
set support8=%ERRORLEVEL%
"%pcretest%" -C pcre16 >NUL
set support16=%ERRORLEVEL%
"%pcretest%" -C utf >NUL
set utf=%ERRORLEVEL%
"%pcretest%" -C ucp >NUL
set ucp=%ERRORLEVEL%
"%pcretest%" -C jit >NUL
set jit=%ERRORLEVEL%

if %support8% EQU 1 (
if not exist testout8 md testout8
if not exist testoutstudy8 md testoutstudy8
if not exist testoutjit8 md testoutjit8
)

if %support16% EQU 1 (
if not exist testout16 md testout16
if not exist testoutstudy16 md testoutstudy16
if not exist testoutjit16 md testoutjit16
)

set do1=no
set do2=no
set do3=no
set do4=no
set do5=no
set do6=no
set do7=no
set do8=no
set do9=no
set do10=no
set do11=no
set do12=no
set do13=no
set do14=no
set do15=no
set do16=no
set do17=no
set do18=no
set do19=no
set do20=no
set all=yes

for %%a in (%*) do (
  set valid=no
  for %%v in (1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20) do if %%v == %%a set valid=yes
  if "!valid!" == "yes" (
    set do%%a=yes
    set all=no
) else (
    echo Invalid test number - %%a!
        echo Usage %0 [ test_number ] ...
        echo Where test_number is one or more optional test numbers 1 through 20, default is all tests.
        exit /b 1
)
)
set failed="no"

if "%all%" == "yes" (
  set do1=yes
  set do2=yes
  set do3=yes
  set do4=yes
  set do5=yes
  set do6=yes
  set do7=yes
  set do8=yes
  set do9=yes
  set do10=yes
  set do11=yes
  set do12=yes
  set do13=yes
  set do14=yes
  set do15=yes
  set do16=yes
  set do17=yes
  set do18=yes
  set do19=yes
  set do20=yes
)

@echo RunTest.bat's pcretest output is written to newly created subfolders named
@echo testout, testoutstudy and testoutjit.
@echo.

set mode=
set bits=8

:nextMode
if "%mode%" == "" (
  if %support8% EQU 0 goto modeSkip
  echo.
  echo ---- Testing 8-bit library ----
  echo.
) else (
  if %support16% EQU 0 goto modeSkip
  echo.
  echo ---- Testing 16-bit library ----
  echo.
)
if "%do1%" == "yes" call :do1
if "%do2%" == "yes" call :do2
if "%do3%" == "yes" call :do3
if "%do4%" == "yes" call :do4
if "%do5%" == "yes" call :do5
if "%do6%" == "yes" call :do6
if "%do7%" == "yes" call :do7
if "%do8%" == "yes" call :do8
if "%do9%" == "yes" call :do9
if "%do10%" == "yes" call :do10
if "%do11%" == "yes" call :do11
if "%do12%" == "yes" call :do12
if "%do13%" == "yes" call :do13
if "%do14%" == "yes" call :do14
if "%do15%" == "yes" call :do15
if "%do16%" == "yes" call :do16
if "%do17%" == "yes" call :do17
if "%do18%" == "yes" call :do18
if "%do19%" == "yes" call :do19
if "%do20%" == "yes" call :do20
:modeSkip
if "%mode%" == "" (
  set mode=-16
  set bits=16
  goto nextMode
)

if %failed% == "yes" (
echo In above output, one or more of the various tests failed!
exit /b 1
)
echo All OK
goto :eof

:runsub
@rem Function to execute pcretest and compare the output
@rem Arguments are as follows:
@rem
@rem       1 = test number
@rem       2 = outputdir
@rem       3 = test name use double quotes
@rem   4 - 9 = pcretest options

if [%1] == [] (
  echo Missing test number argument!
  exit /b 1
)

if [%2] == [] (
  echo Missing outputdir!
  exit /b 1
)

if [%3] == [] (
  echo Missing test name argument!
  exit /b 1
)

set testinput=testinput%1
set testoutput=testoutput%1
if exist %srcdir%\testdata\win%testinput% (
  set testinput=wintestinput%1
  set testoutput=wintestoutput%1
)

echo Test %1: %3
"%pcretest%" %mode% %4 %5 %6 %7 %8 %9 "%srcdir%\testdata\%testinput%">%2%bits%\%testoutput%
if errorlevel 1 (
  echo.          failed executing command-line:
  echo.            "%pcretest%" %mode% %4 %5 %6 %7 %8 %9 "%srcdir%\testdata\%testinput%"^>%2%bits%\%testoutput%
  set failed="yes"
  goto :eof
)

if [%1]==[11] (
  fc /n "%srcdir%\testdata\%testoutput%-%bits%" "%2%bits%\%testoutput%">NUL
) else (
  fc /n "%srcdir%\testdata\%testoutput%" "%2%bits%\%testoutput%">NUL
)
if errorlevel 1 (
  echo.          failed comparison: fc /n "%srcdir%\testdata\%testoutput%" "%2%bits%\%testoutput%"
  if [%1]==[2] (
    echo.
    echo ** Test 2 requires a lot of stack. PCRE can be configured to
    echo ** use heap for recursion. Otherwise, to pass Test 2
    echo ** you generally need to allocate 8 mb stack to PCRE.
    echo ** See the 'pcrestack' page for a discussion of PCRE's
    echo ** stack usage.
    echo.
)
  if [%1]==[3] (
    echo.
    echo ** Test 3 failure usually means french locale is not
    echo ** available on the system, rather than a bug or problem with PCRE.
    echo.
    goto :eof
)

  set failed="yes"
  goto :eof
)

echo.          Passed.
goto :eof

:do1
call :runsub 1 testout "Main functionality - Compatible with Perl 5.8 and above" -q
call :runsub 1 testoutstudy "Test with Study Override" -q -s
if %jit% EQU 1 call :runsub 1 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do2
  call :runsub 2 testout "API, errors, internals, and non-Perl stuff (not UTF-8)" -q
  call :runsub 2 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 2 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do3
  call :runsub 3 testout "Locale-specific features" -q
  call :runsub 3 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 3 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do4
  if %utf% EQU 0 (
  echo Test 4 Skipped due to absence of UTF-%bits% support.
  goto :eof
)
  call :runsub 4 testout "UTF-%bits% support - Compatible with Perl 5.8 and above" -q
  call :runsub 4 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 4 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do5
  if %utf% EQU 0 (
  echo Test 5 Skipped due to absence of UTF-%bits% support.
  goto :eof
)
  call :runsub 5 testout "API, internals, and non-Perl stuff for UTF-%bits% support" -q
  call :runsub 5 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 5 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do6
if %ucp% EQU 0 (
  echo Test 6 Skipped due to absence of Unicode property support.
  goto :eof
)
  call :runsub 6 testout "Unicode property support (Compatible with Perl >= 5.10)" -q
  call :runsub 6 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 6 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do7
if %ucp% EQU 0 (
  echo Test 7 Skipped due to absence of Unicode property support.
  goto :eof
)
  call :runsub 7 testout "API, internals, and non-Perl stuff for Unicode property support" -q
  call :runsub 7 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 7 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do8
  call :runsub 8 testout "DFA matching main functionality" -q -dfa
  call :runsub 8 testoutstudy "Test with Study Override" -q -dfa -s
goto :eof

:do9
  if %utf% EQU 0 (
  echo Test 9 Skipped due to absence of UTF-%bits% support.
  goto :eof
)
  call :runsub 9 testout "DFA matching with UTF-%bits%" -q -dfa
  call :runsub 9 testoutstudy "Test with Study Override" -q -dfa -s
  goto :eof

:do10
  if %ucp% EQU 0 (
  echo Test 10 Skipped due to absence of Unicode property support.
  goto :eof
)
  call :runsub 10 testout "DFA matching with Unicode properties" -q -dfa
  call :runsub 10 testoutstudy "Test with Study Override" -q -dfa -s
goto :eof

:do11
  if NOT %link_size% EQU 2 (
  echo Test 11 Skipped because link size is not 2.
  goto :eof
)
  if %ucp% EQU 0 (
  echo Test 11 Skipped due to absence of Unicode property support.
  goto :eof
)
  call :runsub 11 testout "Internal offsets and code size tests" -q
  call :runsub 11 testoutstudy "Test with Study Override" -q -s
goto :eof

:do12
if %jit% EQU 0 (
  echo Test 12 Skipped due to absence of JIT support.
  goto :eof
)
  call :runsub 12 testout "JIT-specific features - have JIT" -q
goto :eof

:do13
  if %jit% EQU 1 (
  echo Test 13 Skipped due to presence of JIT support.
  goto :eof
)
  call :runsub 13 testout "JIT-specific features - no JIT" -q
goto :eof

:do14
  if NOT %bits% EQU 8 (
  echo Test 14 Skipped when running 16-bit tests.
  goto :eof
)
  copy /Y "%srcdir%\testdata\saved16" testsaved16
  call :runsub 14 testout "Specials for the basic 8-bit library" -q
  call :runsub 14 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 14 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do15
  if NOT %bits% EQU 8 (
  echo Test 15 Skipped when running 16-bit tests.
  goto :eof
)
  if %utf% EQU 0 (
  echo Test 15 Skipped due to absence of UTF-8 support.
  goto :eof
)
  call :runsub 15 testout "Specials for the 8-bit library with UTF-8 support" -q
  call :runsub 15 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 15 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do16
  if NOT %bits% EQU 8 (
  echo Test 16 Skipped when running 16-bit tests.
  goto :eof
)
  if %ucp% EQU 0 (
  echo Test 16 Skipped due to absence of Unicode property support.
  goto :eof
)
  call :runsub 16 testout "Specials for the 8-bit library with Unicode propery support" -q
  call :runsub 16 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 16 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do17
  if NOT %bits% EQU 16 (
  echo Test 17 Skipped when running 8-bit tests.
  goto :eof
)
  copy /Y "%srcdir%\testdata\saved8" testsaved8
  copy /Y "%srcdir%\testdata\saved16LE-1" testsaved16LE-1
  copy /Y "%srcdir%\testdata\saved16BE-1" testsaved16BE-1
  call :runsub 17 testout "Specials for the basic 8-bit library" -q
  call :runsub 17 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 17 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do18
  if NOT %bits% EQU 16 (
  echo Test 18 Skipped when running 8-bit tests.
  goto :eof
)
  if %utf% EQU 0 (
  echo Test 18 Skipped due to absence of UTF-8 support.
  goto :eof
)
  copy /Y "%srcdir%\testdata\saved16LE-2" testsaved16LE-2
  copy /Y "%srcdir%\testdata\saved16BE-2" testsaved16BE-2
  call :runsub 18 testout "Specials for the basic 8-bit library" -q
  call :runsub 18 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 18 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do19
  if NOT %bits% EQU 16 (
  echo Test 19 Skipped when running 8-bit tests.
  goto :eof
)
  if %ucp% EQU 0 (
  echo Test 19 Skipped due to absence of Unicode property support.
  goto :eof
)
  call :runsub 19 testout "Specials for the basic 8-bit library" -q
  call :runsub 19 testoutstudy "Test with Study Override" -q -s
  if %jit% EQU 1 call :runsub 19 testoutjit "Test with JIT Override" -q -s+
goto :eof

:do20
  if NOT %bits% EQU 16 (
  echo Test 20 Skipped when running 8-bit tests.
  goto :eof
)
  call :runsub 20 testout "DFA specials for the basic 16-bit library" -q
  call :runsub 20 testoutstudy "Test with Study Override" -q -s
goto :eof

:conferror
@echo.
@echo Either your build is incomplete or you have a configuration error.
@echo.
@echo If configured with cmake and executed via "make test" or the MSVC "RUN_TESTS"
@echo project, pcre_test.bat defines variables and automatically calls RunTest.bat.
@echo For manual testing of all available features, after configuring with cmake
@echo and building, you can run the built pcre_test.bat. For best results with
@echo cmake builds and tests avoid directories with full path names that include
@echo spaces for source or build.
@echo.
@echo Otherwise, if the build dir is in a subdir of the source dir, testdata needed
@echo for input and verification should be found automatically when (from the
@echo location of the the built exes) you call RunTest.bat. By default RunTest.bat
@echo runs all tests compatible with the linked pcre library but it can be given
@echo a test number as an argument.
@echo.
@echo If the build dir is not under the source dir you can either copy your exes
@echo to the source folder or copy RunTest.bat and the testdata folder to the
@echo location of your built exes and then run RunTest.bat.
@echo.
goto :eof
