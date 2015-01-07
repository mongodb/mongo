@echo off
set TOOLSPKG=%cd%\.gopath\src\github.com\mongodb\mongo-tools
for %%t in (bsondump, common, mongostat, mongofiles, mongoexport, mongoimport, mongorestore, mongodump, mongotop, mongooplog) do echo d | xcopy %cd%\%%t %TOOLSPKG%\%%t /Y /E /S
REM copy vendored libraries to GOPATH
for /f %%v in ('dir /b /a:d "%cd%\vendor\src\*"') do echo d | xcopy %cd%\vendor\src\%%v %cd%\.gopath\src\%%v /Y /E /S
set GOPATH=%cd%\.gopath;%cd%\vendor
