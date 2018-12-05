Projects for various integrated development environments (IDE)
==============================================================

#### Included projects

The following projects are included with the zstd distribution:
- `cmake` - CMake project contributed by Artyom Dymchenko
- `VS2005` - Visual Studio 2005 Project (this project has been moved to the contrib directory and will no longer be supported)
- `VS2008` - Visual Studio 2008 project
- `VS2010` - Visual Studio 2010 project (which also works well with Visual Studio 2012, 2013, 2015)
- `VS_scripts` - command line scripts prepared for Visual Studio compilation without IDE


#### How to compile zstd with Visual Studio

1. Install Visual Studio e.g. VS 2015 Community Edition (it's free).
2. Download the latest version of zstd from https://github.com/facebook/zstd/releases
3. Decompress ZIP archive.
4. Go to decompressed directory then to `projects` then `VS2010` and open `zstd.sln`
5. Visual Studio will ask about converting VS2010 project to VS2015 and you should agree.
6. Change `Debug` to `Release` and if you have 64-bit Windows change also `Win32` to `x64`.
7. Press F7 on keyboard or select `BUILD` from the menu bar and choose `Build Solution`.
8. If compilation will be fine a compiled executable will be in `projects\VS2010\bin\x64\Release\zstd.exe`


#### Projects available within zstd.sln

The Visual Studio solution file `visual\VS2010\zstd.sln` contains many projects that will be compiled to the
`visual\VS2010\bin\$(Platform)_$(Configuration)` directory. For example `zstd` set to `x64` and
`Release` will be compiled to `visual\VS2010\bin\x64_Release\zstd.exe`. The solution file contains the
following projects:

- `zstd` : Command Line Utility, supporting gzip-like arguments
- `datagen` : Synthetic and parametrable data generator, for tests
- `fullbench`  : Precisely measure speed for each zstd inner functions
- `fuzzer` : Test tool, to check zstd integrity on target platform 
- `libzstd` : A static ZSTD library compiled to `libzstd_static.lib`
- `libzstd-dll` : A dynamic ZSTD library (DLL) compiled to `libzstd.dll` with the import library `libzstd.lib`
- `fullbench-dll` : The fullbench program compiled with the import library; the executable requires ZSTD DLL


#### Using ZSTD DLL with Microsoft Visual C++ project

The header file `lib\zstd.h` and the import library
`visual\VS2010\bin\$(Platform)_$(Configuration)\libzstd.lib` are required to compile
a project using Visual C++.

1. The path to header files should be added to `Additional Include Directories` that can
   be found in Project Properties of Visual Studio IDE in the `C/C++` Property Pages on the `General` page.
2. The import library has to be added to `Additional Dependencies` that can
   be found in Project Properties in the `Linker` Property Pages on the `Input` page.
   If one will provide only the name `libzstd.lib` without a full path to the library
   then the directory has to be added to `Linker\General\Additional Library Directories`.

The compiled executable will require ZSTD DLL which is available at
`visual\VS2010\bin\$(Platform)_$(Configuration)\libzstd.dll`. 
