## Prerequisites
WiX Toolset v3.7.1224.0 from http://wixtoolset.org/

## Features
The following are the installer features with the executables they install. 
Each of these features can be installed independently using msiexec /ADDLOCAL 
or using the Installer GUI
	* Server
		* mongod.exe
		* mongod.pdb
	* Client
		* mongo.exe
	* MonitoringTools
		* mongostat.exe
		* mongotop.exe
	* ImportExportTools
		* mongodump.exe
		* mongorestore.exe
		* mongoexport.exe
		* mongoimport.exe
	* Router
		* mongos.exe
		* mongos.pdb
	* MiscellaneousTools
		* bsondump.exe
		* mongofiles.exe
		* mongooplog.exe
		* mongoperf.exe

## Typical install
The typical (default) install, installs all except the Router and 
MiscellaneousTools features.

## Configuring builds
The version, location of binaries and license file can be configured when 
building. Refer to build32bitmsi.bat or build64bitmsi.bat for example
