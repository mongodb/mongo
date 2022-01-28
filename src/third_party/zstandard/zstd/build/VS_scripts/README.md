Command line scripts for Visual Studio compilation without IDE
==============================================================

Here are a few command lines for reference :

### Build with Visual Studio 2013 for msvcr120.dll

Running the following command will build both the `Release Win32` and `Release x64` versions:
```batch
build.VS2013.cmd
```
The result of each build will be in the corresponding `bin\Release\{ARCH}\` folder.

If you want to only need one architecture:
- Win32: `build.generic.cmd VS2013 Win32 Release v120`
- x64: `build.generic.cmd VS2013 x64 Release v120`

If you want a Debug build:
- Win32: `build.generic.cmd VS2013 Win32 Debug v120`
- x64: `build.generic.cmd VS2013 x64 Debug v120`

### Build with Visual Studio 2015 for msvcr140.dll

Running the following command will build both the `Release Win32` and `Release x64` versions:
```batch
build.VS2015.cmd
```
The result of each build will be in the corresponding `bin\Release\{ARCH}\` folder.

If you want to only need one architecture:
- Win32: `build.generic.cmd VS2015 Win32 Release v140`
- x64: `build.generic.cmd VS2015 x64 Release v140`

If you want a Debug build:
- Win32: `build.generic.cmd VS2015 Win32 Debug v140`
- x64: `build.generic.cmd VS2015 x64 Debug v140`

### Build with Visual Studio 2015 for msvcr120.dll

This capability is offered through `build.generic.cmd` using proper arguments:

**For Win32**
```batch
build.generic.cmd VS2015 Win32 Release v120
```
The result of the build will be in the `bin\Release\Win32\` folder.

**For x64**
```batch
build.generic.cmd VS2015 x64 Release v120
```
The result of the build will be in the `bin\Release\x64\` folder.

If you want Debug builds, replace `Release` with `Debug`.

### Build with Visual Studio 2017

`build.VS2017.cmd`, contributed by [@HaydnTrigg](https://github.com/HaydnTrigg),
will build both the `Release Win32` and `Release x64` versions
of the first VS2017 variant it finds, in this priority order :
Enterprise > Professional > Community

Alternatively, it's possible to target a specific version,
using appropriate script, such as `build.VS2017Enterprise.cmd` for example.
