@echo off

if exist "%cd%\.gopath\" rd /s /q "%cd%\.gopath\"
md "%cd%\.gopath\src\github.com\bongodb\"
mklink /J "%cd%\.gopath\src\github.com\bongodb\bongo-tools" "%cd%" >nul 2>&1
set GOPATH=%cd%\.gopath;%cd%\vendor
