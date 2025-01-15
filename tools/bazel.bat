@echo off
setlocal EnableDelayedExpansion

echo common --//bazel/config:running_through_bazelisk > .bazelrc.bazelisk

set REPO_ROOT=%~dp0\..

for %%I in (%REPO_ROOT%) do set cur_dir=%%~nxI

set python="%REPO_ROOT%\bazel-%cur_dir%\external\py_windows_x86_64\dist\python.exe"

if not exist "%python%" (
    echo python prereq missing, using bazel to install python... 1>&2
    "%BAZEL_REAL%" build --config=local @py_windows_x86_64//:all 1>&2
)
SET STARTTIME=%TIME%

set "MONGO_BAZEL_WRAPPER_ARGS=%tmp%\bat~%RANDOM%.tmp"
echo "" > %MONGO_BAZEL_WRAPPER_ARGS%
%python% %REPO_ROOT%/bazel/wrapper_hook.py "%BAZEL_REAL%" %*
for /f "Tokens=* Delims=" %%x in ( %MONGO_BAZEL_WRAPPER_ARGS% ) do set "new_args=!new_args!%%x"
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
    ECHO [WRAPPER_HOOK_DEBUG]: wrapper hook script input args: %*
    ECHO [WRAPPER_HOOK_DEBUG]: wrapper hook script new args: !new_args!
    ECHO [WRAPPER_HOOK_DEBUG]: wrapper hook script took %DURATION%
)

"%BAZEL_REAL%" !new_args!
