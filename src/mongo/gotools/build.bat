@echo off

REM This is not handling tags


if exist "%cd%\vendor\pkg" rd /s /q "%cd%\vendor\pkg"

call set_gopath.bat

if not exist "%cd%\bin" mkdir "%cd%\bin"

for %%i in (bsondump, mongostat, mongofiles, mongoexport, mongoimport, mongorestore, mongodump, mongotop, mongooplog) do (
	echo Building %%i

	go build -o "%cd%\bin\%%i.exe" "%cd%\%%i\main\%%i.go"
)
