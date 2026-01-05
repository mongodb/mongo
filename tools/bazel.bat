@echo off
setlocal EnableDelayedExpansion

rem Enable ANSI escape codes for colors (Windows 10+)
rem Get ESC character for ANSI colors (set once at the start)
for /f %%A in ('echo prompt $E ^| cmd') do set "ESC=%%A"

rem Enable virtual terminal processing for ANSI escape codes (Windows 10+)
rem Create a temporary PowerShell script to enable VT processing
set "VT_SCRIPT=%TEMP%\bazel_vt_%RANDOM%.ps1"
(
    echo [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    echo $signature = @'
    echo     [DllImport("kernel32.dll", SetLastError=true^)]
    echo     public static extern IntPtr GetStdHandle(int nStdHandle^);
    echo     [DllImport("kernel32.dll", SetLastError=true^)]
    echo     public static extern bool GetConsoleMode(IntPtr hConsoleHandle, out uint lpMode^);
    echo     [DllImport("kernel32.dll", SetLastError=true^)]
    echo     public static extern bool SetConsoleMode(IntPtr hConsoleHandle, uint dwMode^);
    echo '@
    echo $type = Add-Type -MemberDefinition $signature -Name Win32Utils -Namespace Console -PassThru
    echo $STD_OUTPUT_HANDLE = -11
    echo $STD_ERROR_HANDLE = -12
    echo $ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
    echo $hOut = $type::GetStdHandle($STD_OUTPUT_HANDLE^)
    echo $hErr = $type::GetStdHandle($STD_ERROR_HANDLE^)
    echo $mode = 0
    echo if ($type::GetConsoleMode($hOut, [ref]$mode^)^) { $null = $type::SetConsoleMode($hOut, $mode -bor $ENABLE_VIRTUAL_TERMINAL_PROCESSING^) }
    echo if ($type::GetConsoleMode($hErr, [ref]$mode^)^) { $null = $type::SetConsoleMode($hErr, $mode -bor $ENABLE_VIRTUAL_TERMINAL_PROCESSING^) }
) > "%VT_SCRIPT%"
>nul 2>&1 powershell -NoProfile -ExecutionPolicy Bypass -File "%VT_SCRIPT%"
del "%VT_SCRIPT%" >nul 2>&1

set REPO_ROOT=%~dp0..

echo common --//bazel/config:running_through_bazelisk > .bazelrc.bazelisk

REM Write a compressed execution log to a file for EngFlow to pick up for more detailed analysis.
rem echo common --execution_log_compact_file=%REPO_ROOT:\=/%/.tmp/bazel_execution_log.binpb.zst > .bazelrc.exec_log_file

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
if !skip_python!=="0" if !current_bazel_command!=="info" set skip_python="1"

if !skip_python!=="1" (
    "%BAZEL_REAL%" %*
    exit /b %ERRORLEVEL%
)

rem === Set up logging for SLOW_PATH (equivalent to bash SLOW_PATH=1) ===
rem Where the log will be stored
set "LOG_DIR=%REPO_ROOT%\.bazel_logs"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"
set "LOGFILE=%LOG_DIR%\bazel_wrapper_%DATE:/=_%_%TIME::=_%_%RANDOM%.log"

rem Set up environment variables for terminal output (for engflow_check.py)
rem On Windows, we use CON device for console output
rem Note: Windows doesn't support file descriptor duplication like Unix,
rem so we'll set these to indicate console output should go to CON
set "MONGO_WRAPPER_STDOUT_FD=CON"
set "MONGO_WRAPPER_STDERR_FD=CON"

rem === Start timing ===
set STARTTIME=%TIME%

rem === Capture output to logfile starting now ===
rem Note: We redirect Python installation and wrapper_hook.py output to logfile

REM find existing python installs
set "python="
if exist "%REPO_ROOT%\bazel-%cur_dir%" (
    call :find_pyhon
)

if not defined python (
    (
        echo python prereq missing, using bazel to install python...
        "%BAZEL_REAL%" build --bes_backend= --bes_results_url= --workspace_status_command= @py_windows_x86_64//:all
        if !ERRORLEVEL! NEQ 0 (
            "%BAZEL_REAL%" build --config=local --workspace_status_command= @py_windows_x86_64//:all
            if !ERRORLEVEL! NEQ 0 (
                if "%CI%"=="" if "%MONGO_BAZEL_WRAPPER_FALLBACK%"=="" (
                    call :cleanup_logfile
                    exit /b !ERRORLEVEL!
                )
                echo wrapper script failed to install python! falling back to normal bazel call...
                "%BAZEL_REAL%" %*
                set "fallback_exit=!ERRORLEVEL!"
                call :cleanup_logfile
                exit /b !fallback_exit!
            )
        )
    ) > "%LOGFILE%" 2>&1
    if !ERRORLEVEL! NEQ 0 (
        echo %ESC%[1;31mERROR:%ESC%[0m Python installation failed:
        type "%LOGFILE%"
        call :cleanup_logfile
        exit /b !ERRORLEVEL!
    )
)

rem After install, locate python again
if not defined python (
    call :find_pyhon
)

rem extra safety: bail if still not found
if not defined python if not exist "!python!" (
    echo %ESC%[1;31mERROR:%ESC%[0m Could not locate wrapper Python interpreter. 1>&2
    call :cleanup_logfile
    exit /b 1
)

rem === Call Python wrapper, log to file ===
set "MONGO_BAZEL_WRAPPER_ARGS=%tmp%\bat~%RANDOM%.tmp"
echo "" > %MONGO_BAZEL_WRAPPER_ARGS%

rem Print info message to terminal (equivalent to bash echo to FD 4)
echo %ESC%[0;32mINFO:%ESC%[0m running wrapper hook... 1>&2

(
    "%python%" %REPO_ROOT%/bazel/wrapper_hook/wrapper_hook.py "%BAZEL_REAL%" %*
) >> "%LOGFILE%" 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo %ESC%[1;31mERROR:%ESC%[0m Python installation failed:
    type "%LOGFILE%"
    call :cleanup_logfile
    exit /b !ERRORLEVEL!
)

set "exit_code=%ERRORLEVEL%"

rem Linter fails preempt bazel run (exit code 3)
if %exit_code% EQU 3 (
    echo %ESC%[0;31mERROR:%ESC%[0m Linter run failed, see details above 1>&2
    echo %ESC%[0;32mINFO:%ESC%[0m Run the following to try to auto-fix the errors: 1>&2
    echo. 1>&2
    echo bazel run lint --fix 1>&2
    call :cleanup_logfile
    exit /b %exit_code%
)

rem Calculate duration for summary (equivalent to bash print_summary)
set ENDTIME=%TIME%
FOR /F "tokens=1-4 delims=:.," %%a IN ("%STARTTIME%") DO (
   SET /A "start=(((%%a*60)+1%%b %% 100)*60+1%%c %% 100)*100+1%%d %% 100"
)
FOR /F "tokens=1-4 delims=:.," %%a IN ("%ENDTIME%") DO (
   SET /A "end=(((%%a*60)+1%%b %% 100)*60+1%%c %% 100)*100+1%%d %% 100"
)
SET /A elapsed=end-start
SET /A hh=elapsed/(60*60*100), rest=elapsed%%(60*60*100), mm=rest/(60*100), rest%%=60*100, ss=rest/100, cc=rest%%100
IF %hh% lss 10 SET hh=0%hh%
IF %mm% lss 10 SET mm=0%mm%
IF %ss% lss 10 SET ss=0%ss%
IF %cc% lss 10 SET cc=0%cc%

if %exit_code% NEQ 0 (  
    echo %ESC%[1;31mERROR:%ESC%[0m wrapper hook failed: 1>&2
    type "%LOGFILE%" 1>&2
    
    if "%CI%"=="" if "%MONGO_BAZEL_WRAPPER_FALLBACK%"=="" (
        call :cleanup_logfile
        exit /b %exit_code%
    )
    echo wrapper script failed! falling back to normal bazel call... 1>&2  
    "%BAZEL_REAL%" %*  
    set "fallback_exit=%ERRORLEVEL%"
    call :cleanup_logfile
    exit /b %fallback_exit%
)

rem === Read new args back in ===
set "new_args="
for /F "delims=" %%a in (%MONGO_BAZEL_WRAPPER_ARGS%) do (
    set str="%%a"
    call set str=!str: =^ !
    set "new_args=!new_args! !str!"
)
del %MONGO_BAZEL_WRAPPER_ARGS%

if "%MONGO_BAZEL_WRAPPER_DEBUG%"=="1" (
    echo [WRAPPER_HOOK_DEBUG]: wrapper hook script input args: %* 1>&2
    echo [WRAPPER_HOOK_DEBUG]: wrapper hook script new args: !new_args! 1>&2
    echo [WRAPPER_HOOK_DEBUG]: wrapper hook script took %mm%m and %ss%.%cc%s 1>&2
)

"%BAZEL_REAL%" !new_args!
set "bazel_exit=%ERRORLEVEL%"
call :cleanup_logfile
exit /b %bazel_exit%


:: Functions
:find_pyhon
dir %REPO_ROOT% | C:\Windows\System32\find.exe "bazel-%cur_dir%" > %REPO_ROOT%\tmp_bazel_symlink_dir.txt
for /f "tokens=2 delims=[" %%i in (%REPO_ROOT%\tmp_bazel_symlink_dir.txt) do set bazel_real_dir=%%i
del %REPO_ROOT%\tmp_bazel_symlink_dir.txt
set bazel_real_dir=!bazel_real_dir:~0,-1!
set "python=!bazel_real_dir!\..\..\external\_main~setup_mongo_python_toolchains~py_windows_x86_64\dist\python.exe"
exit /b 0  

:cleanup_logfile
if defined LOGFILE if exist "!LOGFILE!" del "!LOGFILE!" >nul 2>&1
goto :eof
