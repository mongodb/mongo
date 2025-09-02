# USING MSYS2 (WINDOWS) CMAKE, MESON, CONFIGURE

    Created 7 October 2023
    Updated 19 December 2023

msys2 provides an environment much like posix/unix/linux
with programs precompiled for use on windows
installed in its own set of locations.

Do not use the -DWALL option to cmake or
--enable-wall to configure or the meson
equivalent as it causes a minor warning
about <b>I64u</b> , from gcc
(treated as an error).

When using configure note that building a static
library libdwarf.a is not supported. A static
libdwarf.a can be built with meson or cmake.

### Shared library builds
Libdwarf builds from configure
and cmake  are static (archive) library builds
by default.
Meson builds to a shared library by default.

On msys2 with cmake one can generate a shared library
build with:

    -DBUILD_SHARED=YES -DBUILD_NON_SHARED=NO


With the following on the meson command line
one can build libdwarf as an archive and dwarfdump and the
programs built will use the static library.

    --default-library static


The meson default is shared and can be explicitly
chosen by:

    --default-library shared

configure will only allow generation of shared library
builds, while for cmake and meson one can choose
whether to build a shared library or a static (archive) library
(libdwarf.a).

On msys2 with configure one gets a shared library build with:

    --enable-shared --disable-static

### NOTE on linking against libdwarf.a

If you are are linking code against a static
library libdwarf.a you must arrange to define the
macro LIBDWARF_STATIC in compiling your code that
does a #include "libdwarf.h".

To pass LIBDWARF_STATIC to the preprocessor with Visual Studio:

    right click on a project name
    In the contextual menu, click on Properties at the very bottom
    In the new window, double click on C/C++
    On the right, click on Preprocessor definitions
    The is a small down arrow on the right, click on it,
    then click on Modify
    Add LIBDWARF_STATIC to the values
    Click on OK to close the windows

### Building with Visual Studio on Windows

While Windows Visual Studio is not
a supported environment the following
may be of interest to some.

Building in Windows Visual Studio is fairly
straightforward.  We'll use
the simplest possible setup (we hope).
Using VS buttons and settings do:

1. Main Panel: Click Git. Subpanel: click Clone Repository
   In the subpanel fill in the clone source:
   https://github.com/davea42/libdwarf-code
   and the directory to place the clone.

2. Main Panel: Click Git. Options list: click Cmake Setup.
   A Folder View panel (should show up) Click on the top-level CMakeLists.txt file.

3. Main Panel:  Click Build. Options list: click Build All

Expect a few warnings because the source uses Posix rather
than Windows-only calls where applicable.
A way to eliminate such warnings using VS is to edit
  CMakeSettings_schema.json

1. Click on Project (top level menu)

2. Click on "CMake Settings for libdwarf"

3. In the settings panel click on "Edit JSON"

4. In the "buildCommandArgs" line
   <pre>
   Change 
     "buildCommandArgs": ""
   to
     "buildCommandArgs": "-D_CRT_SECURE_NO_WARNINGS"
   </pre>

## Setting up msys2 on Windows

We suggest you use meson for  msys2 builds.

Direct your browser to msys2.org

Download a recent .exe from the downloads page.
For example msys2-x86_64-20230718.exe
Execute it and follow the instructions on msys2.org

A straightforward way to use the tests etc in
the source is to find the appropriate MSYS2
shell program and put a link to it on the desktop.
The Windows list of applications will show MSYS2
and under that category there will be a list of
candidates to use.

The useful candidates on x86_64 are are

    MSYS2 MINGW64  The compiler is gcc
    MSYS2 CLANG64  The compiler is clang

the following should get you sufficient files for
building and testing all the build mechanisms:

    basics
    pacman -Suy
    pacman -S base-devel git autoconf automake libtool
    pacman -S mingw-w64-x86_64-python3
    pacman -S mingw-w64-x86_64-toolchain
    pacman -S mingw-w64-x86_64-zlib
    pacman -S mingw-w64-x86_64-zstd
    pacman -S mingw-w64-x86_64-doxygen

    extras for meson/cmake
    pacman -S mingw-w64-x86_64-meson
    pacman -S mingw-w64-x86_64-cmake
    pacman -S mingw-w64-x86_64-python3-pip

    To create a distribution one needs xz:
    pacman -S mingw-w64-x86_64-xz

    to list packages
    pacman -Q
    to remove packages
    pacman -R  <packagename>

## Ninja speed

cmake will generate ninja makefiles on mingw by default, add
'-G "Unix Makefiles"' to the cmake command line to
generate makefiles for gnu make, but we suggest you
use "-G Ninja" for speed and clarity..

## Basic Testing of libdwarf

This checks for the existence critical executables
such as cmake,meson,and ninja and only runs builds
that could work.

    sh scripts/allsimplebuilds.sh

## Set a Prefix for test installs

To get a usable set of executables
set a prefix (for cmake,
-DCMAKE_INSTALL_PREFIX=$HOME/bin
presuming the bin directory
is something in your $PATH in msys2.
Set an appropriate prefix whichever
build tool you use.

    ninja install
    cp src/bin/dwarfdump/dwarfdump.conf to $HOME
    # then
    dwarfdump.exe
    # which will give a short message  about
    # No object file provided. In which case
    # dwarfdump is usable.

