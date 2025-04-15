@echo off
setlocal EnableDelayedExpansion

echo common --//bazel/config:running_through_bazelisk > .bazelrc.bazelisk

set REPO_ROOT=%~dp0..

for %%I in (%REPO_ROOT%) do set cur_dir=%%~nxI

REM skip python if the bazel command type is in a known list of commands to skip
set skip_python="0"
set bazelCommandCount=0
for /F "delims=" %%a in (%REPO_ROOT%\bazel\wrapper_hook\bazel_commands.commands) do (
    set /A bazelCommandCount+=1
    set "bazel_commands[!bazelCommandCount!]=%%~a"
)

set argCount=0
for %%x in (%*) do (
   set /A argCount+=1
   set "argVec[!argCount!]=%%~x"
)

set current_bazel_command=""
for /L %%i in (1,1,%argCount%) do (
  for /L %%j in (1,1,%bazelCommandCount%) do (
    if "!bazel_commands[%%j]!"=="!argVec[%%i]!" (
      set current_bazel_command="!argVec[%%i]!"
      goto :found_command
    )
  )
)
:found_command
if !current_bazel_command!=="" set skip_python="1"

if !skip_python!=="0" if !current_bazel_command!=="clean" set skip_python="1"
if !skip_python!=="0" if !current_bazel_command!=="version" set skip_python="1"
if !skip_python!=="0" if !current_bazel_command!=="shutdown" set skip_python="1"

if !skip_python!=="1" (
    "%BAZEL_REAL%" %*
    exit %ERRORLEVEL%
)
REM find existing python installs
set python=""
if exist %REPO_ROOT%\bazel-%cur_dir% (
     call :find_pyhon
)
if not exist "!python!" (
    echo python prereq missing, using bazel to install python... 1>&2
    "%BAZEL_REAL%" build --config=local @py_windows_x86_64//:all 1>&2
    
    if %ERRORLEVEL% NEQ 0 (
        if "%CI%"=="" if "%MONGO_BAZEL_WRAPPER_FALLBACK%"=="" exit %ERRORLEVEL%
        echo wrapper script failed to install python! falling back to normal bazel call... 1>&2
        "%BAZEL_REAL%" %*
        exit %ERRORLEVEL%
    )
)

if not exist "!python!" (
    call :find_pyhon
)

SET STARTTIME=%TIME%

set "MONGO_BAZEL_WRAPPER_ARGS=%tmp%\bat~%RANDOM%.tmp"
echo "" > %MONGO_BAZEL_WRAPPER_ARGS%
%python% %REPO_ROOT%/bazel/wrapper_hook/wrapper_hook.py "%BAZEL_REAL%" %* 1>&2
if %ERRORLEVEL% NEQ 0 (
    if "%CI%"=="" if "%MONGO_BAZEL_WRAPPER_FALLBACK%"=="" exit %ERRORLEVEL%
    echo wrapper script failed! falling back to normal bazel call... 1>&2
    "%BAZEL_REAL%" %*
    exit %ERRORLEVEL%
)

for /F "delims=" %%a in (%MONGO_BAZEL_WRAPPER_ARGS%) do (
    set str="%%a"
    call set str=!str: =^ !
    set "new_args=!new_args! !str!"
)
del %MONGO_BAZEL_WRAPPER_ARGS%

REM Final Calculations
SET ENDTIME=%TIME%
FOR /F "tokens=1-4 delims=:.," %%a IN ("%STARTTIME%") DO (
   SET /A "start=(((%%a*60)+1%%b %% 100)*60+1%%c %% 100)*100+1%%d %% 100"
)

FOR /F "tokens=1-4 delims=:.," %%a IN ("%ENDTIME%") DO (
   SET /A "end=(((%%a*60)+1%%b %% 100)*60+1%%c %% 100)*100+1%%d %% 100"
)

REM Calculate the elapsed time by subtracting values
SET /A elapsed=end-start

REM Format the results for output
SET /A hh=elapsed/(60*60*100), rest=elapsed%%(60*60*100), mm=rest/(60*100), rest%%=60*100, ss=rest/100, cc=rest%%100
IF %hh% lss 10 SET hh=0%hh%
IF %mm% lss 10 SET mm=0%mm%
IF %ss% lss 10 SET ss=0%ss%
IF %cc% lss 10 SET cc=0%cc%
SET DURATION=%mm%m and %ss%.%cc%s

if "%MONGO_BAZEL_WRAPPER_DEBUG%"=="1" ( 
    ECHO [WRAPPER_HOOK_DEBUG]: wrapper hook script input args: %* 1>&2
    ECHO [WRAPPER_HOOK_DEBUG]: wrapper hook script new args: !new_args! 1>&2
    ECHO [WRAPPER_HOOK_DEBUG]: wrapper hook script took %DURATION% 1>&2
)

"%BAZEL_REAL%" !new_args!

EXIT /B %ERRORLEVEL%

:: Functions

:find_pyhon
dir %REPO_ROOT% | C:\Windows\System32\find.exe "bazel-%cur_dir%" > %REPO_ROOT%\tmp_bazel_symlink_dir.txt
for /f "tokens=2 delims=[" %%i in (%REPO_ROOT%\tmp_bazel_symlink_dir.txt) do set bazel_real_dir=%%i
del %REPO_ROOT%\tmp_bazel_symlink_dir.txt
set bazel_real_dir=!bazel_real_dir:~0,-1!
set python="!bazel_real_dir!\..\..\external\_main~setup_mongo_python_toolchains~py_windows_x86_64\dist\python.exe" 
EXIT /B 0