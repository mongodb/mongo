# This is libdwarf README.md

Updated 18 May 2025

## Goal
Libdwarf has been focused for years
on both providing access to DWARF2 through
DWARF5 data in a portable
way while also detecting and reporting
if the DWARF is corrupted and avoiding
run-time crashes or memory leakage regardless
how corrupted the DWARF being read may be.
The intent is to provide ABI independent
access to DWARF data and ensure that data
returned  by the library is meaningful.

When the DWARF6 standard is released by the DWARF committee
support will be added (as soon as
reasonably possible) to libdwarf for all
changes/additions while continuing to support
previous versions.

Libdwarf reads files from disk, it does not
read running programs or running shared objects.

See REQUIREMENTS below for information
on what libraries are needed.

## github actions
ci runs builds on Linux, Freebsd, msys2, and MacOS
using configure,cmake, and meson.

[![ci](https://github.com/davea42/libdwarf-code/actions/workflows/test.yml/badge.svg)](https://github.com/davea42/libdwarf-code/actions/workflows/test.yml)

[![OpenSSF Best Practices](https://bestpractices.coreinfrastructure.org/projects/7275/badge)](https://bestpractices.coreinfrastructure.org/projects/7275)

    Version 2.1.0  Released 20 July      2025.
    Version 2.0.0  Released 20 May       2025.
    Version 0.12.0 Released  2 April     2025.
    Version 0.11.1 Released  1 December  2024.
    Version 0.11.0 Released 15 August    2024.
    Version 0.10.1 Released  1 July      2024.
    Version 0.9.2  Released  2 April     2024.
    Version 0.9.1  Released 27 January   2024.

## NOTE on linking against libdwarf.a

If you are linking code against a static
library libdwarf.a You must arrange to  define the
macro LIBDWARF_STATIC in compiling your code that
does a #include "libdwarf.h".
See also READMEwin-msys2.md

## REQUIREMENTS from a libdwarf<name>.tar.xz

Mentioning some that might not be automatically
in your base OS release. Restricting attention
here to just building libdwarf and dwarfdump.

Nothing in the project requires or references
elf.h, libelf.h, or libelf as of 29 June 2023,
version 0.8.0.

If the objects you work with do not have
section content compressed
with zlib(libz) or libzstd
neither those libraries nor their header files
are required for building/using
libdwarf/dwarfdump.

No libraries other than libc are needed to build or
use libdwarf or dwarfdump.

    Ubuntu:
    sudo apt install xz pkgconf zlib1g zlib1g-dev libzstd1
    sudo apt install libzstd-dev

    # Use of libzstd1 is new in 0.4.3
    # zlib1g zlib1g-dev libzstd1 are all optional but
    # are required to read any DWARF data in compressed
    # sections. libzstd1 was used by many linux system utilities
    # in Ubuntu 20.04.
    optional add: cmake meson ninja doxygen

    FreeBSD:
    pkg install bash xz python3 gmake liblz4 zstd
    # libzstd is likely in /usr/local/lib and zstd.h
    # in /usr/local/include and the compiler may not look there
    # by default. All will still build fine without it and
    # without lzib too, though compressed DWARF sections
    # may not be readable.
    # example:
    CPPFLAGS="-I/usr/local/include/" LDFLAGS="-L/usr/local/lib" ./configure
    optional add: binutils cmake meson ninja doxygen

    Ensure that all the needed programs are in $PATH,
    including python3.
    # candidate command to make python3 visible (as root)
    # something like:
    cd /usr/local/bin ; ln -s python3.9 python3

## BUILDING from a libdwarf<name>.tar.xz

This is always recommended as it's not necessary
to have GNU autotools installed.
These examples show doing a build in a directory
different than the source as that is generally
recommended practice.

### GNU configure/autotools build

Note: if you get a build failure that mentions
something about test/ and missing .Po object files
add --disable-dependency-tracking to the configure
command.

    rm -rf /tmp/build
    mkdir /tmp/build
    cd /tmp
    tar xf <path to>/libdwarf-0.4.2.tar.xz
    cd  /tmp/build
    /tmp/libdwarf-0.4.2/configure
    make
    make check

### cmake build

READMEcmake.md has details on the available cmake options.

We suggest that you will find meson a more satisfactory
tool.

### meson build

    meson 0.45.1  on Ubuntu 18.04 fails.
    meson 0.55.2  on Ubuntu 20.04 works.
    meson 0.60.3  on Freebsd 12.2 and Freebsd 13.0 works.

See READMEwin-msys2.md for the mingw64 msys2 packages to install
and the command(s) to do that in msys2.
The tools listed there are also for msys2 meson and
autotools/configure.

The msys2 meson ninja install not only installs libdwarf-0.dll
and dwarfdump.exe it updates the executables in
the build tree linking to that dll so all such
executables in the build tree work too.

For example (all build environments):

    meson /tmp/libdwarf-0.4.2
    ninja
    ninja install
    ninja test

For a faster build, adding additional checks:

    export CFLAGS="-g -pipe"
    export CXXFLAGS="-g -pipe"
    meson /tmp/libdwarf-0.4.2 -Ddwarfexample=true
    ninja -j8
    ninja install
    ninja test

To build a libdwarf that does not refer to or link with
decompression libraries zstd or zlib, add the meson
option  "-Ddecompression=false"

## BUILDING example showing simple builds:

This checks for the existence critical executables
such as cmake,meson,and ninja and only runs builds
that could work. Useful in any supported
environment.

    sh scripts/allsimplebuilds.sh

## BUILDING on linux from a git clone with configure/autotools

Ignore this section if using meson (or cmake).

This is not recommended as it requires you have more
software installed.

For Ubuntu configure, install additional packages:

    autoconf
    automake
    libtool
    pkg-config

For FreeBSD configure, install additional packages:
    autoconf
    automake
    libtool
    pkgconf

Here we assume the source is in  a directory named
/path/to/code

For example, on Ubuntu 20.04
```
    sudo apt-get install autoconf libtool pkg-config
```

Note: if you get a build failure that mentions
something about test/ and missing .Po object files
add --disable-dependency-tracking to the configure
command.

Using the source/build directories from above as examples,
do :

    # Standard Linux Build
    cd /path/to/code
    sh autogen.sh
    cd /tmp/build
    /path/to/code/configure
    make
    make check

## BUILDING on MacOS from a git clone configure
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    brew install autoconf automake libtool
    # Then use the  Standard Linux Build lines above.

### Options to meson on Linux/Unix

For the basic configuration options list , do:
    meson configure /path/to/code

To set options and show the resulting actual options:

    # Here  just setting one option.
    meson setup  -Ddwarfexample=true  .  /home/davea/dwarf/code
    meson configure .

The meson configure output is very wide (just letting you know).

### Options to configure/autotools on Linux/Unix

For the full options list , do:

    /path/to/code/configure --help

By default configure compiles and uses libdwarf.a.
With `--enable-shared --disable-static"
appended to the configure step,
libdwarf.so is built and the runtimes
built will reference libdwarf.so.

As of version 0.9.1 the configure option
"--disable-decompression" tells the build to compile
libdwarf and dwarfdump with no reference to the zlib or
zstd libraries.

If you get a build failure that mentions
something about test/ and missing .Po object files
add --disable-dependency-tracking to the configure
command. With that option do not assume you can
alter source files and have make rebuild all
necessary.

See:
https://www.gnu.org/savannah-checkouts/gnu/automake/history/automake-history.html#Dependency-Tracking-Evolution


Other options of possible interest:

    --enable-wall to turn on compiler diagnostics
    --enable-dwarfexample to compile the example programs.

    configure -h     shows the options available.

Sanity checking:

gcc has some checks that can be done at runtime.
-fsanitize=undefined is turned on  for
configure by --enable-sanitize

### Options to meson on linux MacOS Windows (Msys2)

As of 0.9.0 meson builds default to be
shared-library builds.
These options go on the meson setup command line.
the default can be explicitly chosen with:

    --default-library shared

A static libdwarf (archive) libdwarf.a can be built with

    --default-library static

By default compiler warnings are errors.  Add the following
to let compilations continue:

    -Dwerror=false

By default compiles look for C/C++ language issues.
Add the following to add gcc -fsanitize checking
in the build to catch various memory errors
(the generated code is larger and slower than normal).

    -Dsanitize=true


### Options to configure on Windows (Msys2)

All libdwarf builds are automatically shared object (dll)
builds. No static libdwarf.a can be built.
If you need static libdwarf.a use meson or cmake.

Has the same meson setup reporting as on Linux (above).

See READMEwin-msys2.md

### Distributing via configure/autotools

When ready to create a new source distribution do
a build and then

    make distcheck

# INCOMPATIBILITIES. Changes to interfaces

### Comparing libdwarf-0.11.0 to libdwarf-0.9.1

Added  dwarf_get_ranges_baseaddress() to the API.

### Comparing libdwarf-0.9.1 to libdwarf-0.9.0

Altered the type of the return value of
dwarf_die_abbrev_code() and dwarf_get_section_count()
from int to Dwarf_Unsigned for consistency (should always
have been this way).

### Comparing libdwarf-0.9.0 to libdwarf-0.8.0

New interfaces allow full support for
Mach-O (Apple) universal binaries:
dwarf_init_path_a(), dwarf_init_path_dl_a(), and
dwarf_get_universalbinary_count().

### Comparing libdwarf-0.8.0 to libdwarf-0.7.0

The default build (with meson) is shared-library.
to build with static (archive) libdwarf
add

    --default-library static

to the meson command line (applies to meson
builds in Linux,Macos, and Windows-mingw).

On Windows-mingw one can build static libdwarf
when using cmake or meson (configure will
not allow a static libdwarf to be built
on Windows).

See Options to meson on Windows (Msys2) above.

### Comparing libdwarf-0.7.0 to libdwarf-0.6.0
struct Dwarf\_Obj\_Access\_Methods\_a\_s  changed
for extended ELF so the library can handle section count values
larger than 16bits.
dwarf\_dnames\_abbrev\_by\_code() and
dwarf\_dnames\_abbrev\_form\_by\_index()
were removed from the API, better alternatives
already existed.

### Comparing libdwarf-0.6.0 to libdwarf-0.5.0
The dealloc required by dwarf\_offset\_list()
was wrong, use dwarf\_dealloc(dbg, offsetlistptr, DW_DLA_UARRAY).
The function dwarf\_dietype\_offset() has a revised
argument list so it can work correctly with DWARF4.
dwarf\_get\_pubtypes() and similar changed
to eliminate messy code duplication.
Fixed memory leaks and treatment of DW\_FORM\_strx3
and DW\_FORM\_addrx3.

### Comparing libdwarf-0.5.0 to libdwarf-0.4.2
dwarf\_get\_globals() is compatible but it now
returns data from .debug\_names in addition
to .debug\_pubnames (either or both
could be in an object file).
New function dwarf\_global\_tag\_number()
makes the data from .debug\_names a bit
more useful (if a library user wants it).
Three new functions were added to enable
printing of the .debug_addr section
independent of other sections
and the new dwarfdump option --print-debug-addr
prints that section.

### Comparing libdwarf-0.4.2 to libdwarf-0.4.1
No incompatibilities.

### Comparing libdwarf-0.4.1 to libdwarf-0.4.0
Added a new function dwarf\_suppress\_debuglink\_crc()
which speeds up gnu debuglink (only use it if
you are sure the debuglink name-check alone is sufficient).

### Comparing libdwarf-0.4.0 to libdwarf-0.3.4
A few  dealloc() functions changed name to have
a consistent pattern for all such.
Access to the DWARF5 .debug\_names section
is now fully implemented.

See the <strong>Recent Changes</strong> section in
libdwarf.pdf (in the release).

[dwhtml]: https://www.prevanders.net/libdwarfdoc/index.html
[dwpdf]: https://www.prevanders.net/libdwarf.pdf

Or see the latest online html version [dwhtml] for the details..
Or see (via download) the latest pdf html version [dwpdf].

Notice the table of contents at the right edge of the html page.

## Reading DWARF from memory

If one has DWARF bytes in memory or in a
kind of file system libdwarf cannot understand
one should use

    dwarf_object_init_b()
    ...call libdwarf functions...
    dwarf_object_finish()

and create source to provide
functions and data for the three struct
types:

    struct Dwarf_Obj_Access_Interface_a_s
    struct Dwarf_Obj_Access_Methods_a_s
    struct Dwarf_Obj_Access_Section_a_s

These functions and structs now seem complete
(unlike the earlier libdwarf versions), hence
the name and content changes.

For a worked out example of reading DWARF direct from memory
with no file system involved
see

    src/bin/dwarfexample/jitreader.c

and see the html [dwhtml] (www.prevanders.net/libdwarfdoc/index.html).

The latest pdf is [dwpdf] (www.prevanders.net/libdwarf.pdf)

David Anderson.
