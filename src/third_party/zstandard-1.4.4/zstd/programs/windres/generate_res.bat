@echo off
REM http://stackoverflow.com/questions/708238/how-do-i-add-an-icon-to-a-mingw-gcc-compiled-executable

where /q windres.exe
IF ERRORLEVEL 1 (
    ECHO The windres.exe is missing. Ensure it is installed and placed in your PATH.
    EXIT /B
) ELSE (
    windres.exe -I ../lib -I windres -i windres/zstd.rc -O coff -F pe-x86-64 -o windres/zstd64.res
    windres.exe -I ../lib -I windres -i windres/zstd.rc -O coff -F pe-i386 -o windres/zstd32.res
)
