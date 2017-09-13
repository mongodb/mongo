@echo off

setlocal EnableDelayedExpansion

set GOPATH=%cd%\vendor

for /F "eol=; tokens=1,2,3" %%i in (Godeps) do (
	set package=%%i
	set version=%%j
	set dest=%%k
	echo Getting package !package!

	if not "!dest!"=="" (
		set dest=!package!
		set package=%%k
	)

	go get -u -d "!package!" >nul 2>&1
	echo Setting package to version !version!
	cd "%GOPATH%\src\!package!"
	git checkout !version! >nul 2>&1

	if not "!dest!"=="" (
		cd "%GOPATH%"
		if exist "%GOPATH%\src\!dest!" rd /s /q "%GOPATH%\src\!dest!"
		xcopy "%GOPATH%\src\!package!" "%GOPATH%\src\!dest!" /Y /S /I >nul 2>&1
		rd /s /q "%GOPATH%\src\!package!"
	)
)

endlocal
