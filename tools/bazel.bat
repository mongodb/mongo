@echo off
setlocal EnableDelayedExpansion

echo common --//bazel/config:running_through_bazelisk > .bazelrc.bazelisk

set REPO_ROOT=%~dp0..

for %%I in (%REPO_ROOT%) do set cur_dir=%%~nxI

set bazel_python="%REPO_ROOT%\bazel-%cur_dir%\external\_main~setup_mongo_python_toolchains~py_windows_x86_64\dist\python.exe"
set compdb_python="%REPO_ROOT%\.compiledb\compiledb-%cur_dir%\external\py_windows_x86_64\dist\python.exe"
set python=%bazel_python%
if not exist "%python%" (
    set python=%compdb_python%
)
if not exist "%python%" (
    echo python prereq missing, using bazel to install python... 1>&2
    "%BAZEL_REAL%" build --config=local @py_windows_x86_64//:all 1>&2
    
    if %ERRORLEVEL% NEQ 0 (
        if "%CI%"=="" if "%MONGO_BAZEL_WRAPPER_FALLBACK%"=="" exit %ERRORLEVEL%
        echo wrapper script failed to install python! falling back to normal bazel call... 1>&2
        "%BAZEL_REAL%" %*
        exit %ERRORLEVEL%
    )
)
set python=%bazel_python%
if not exist "%python%" (
    set python=%compdb_python%
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
