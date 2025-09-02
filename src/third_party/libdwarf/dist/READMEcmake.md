# Cmake on Unix/linux/MacOS/FreeBSD/OpenBSD
Created 26 April 2019
Updated 19 December 2023

Consider switching entirely to meson for your build.

Unless a shared library is specifically requested
cmake builds a static library: libdwarf.a

For cmake, ignore the autogen.sh
script in the base source directory, autogen.sh
is only for configure.

By default cmake builds just libdwarf and dwarfdump
and libdwarf is a static (archive) library.
To switch to a shared library output with cmake add
the following to the cmake command:

    -DBUILD_SHARED=YES -DBUILD_NON_SHARED=NO

Lets assume the base directory of the the libdwarf source in a
directory named 'code' inside the directory '/path/to/' Always
arrange to issue the cmake command in an empty directory.

If you are building a Shared Library you may need to
install before running tests to allow the tests to work
Use -DCMAKE_INSTALL_PREFIX=/some/path  to chose
the install path.
You can install in any temporary directory or
in system directories and the tests will work.

    # build the fast way
    mkdir /tmp/cmbld
    cd /tmp/cmbld
    cmake -G Ninja -DDO_TESTING:BOOL=TRUE /path/to/code
    ninja
    ninja test

    # slower build
    mkdir /tmp/cmbld
    cd /tmp/cmbld
    cmake -G "Unix Makefiles" -DDO_TESTING:BOOL=TRUE  /path/to/code
    make
    ctest -R self

It is best to specify -G explicitly since some versions of cmake
seem to have a different default for -G than others.

On Windows msys2 -DBUILD_SHARED=YES will build libdwarf.dll.

To show all the available cmake options for 'code':

    cmake -L /path/to/code

For dwarfexample:

    cmake -G Ninja -DBUILD_DWARFEXAMPLE=ON /path/to/code
    make

or

    cmake -G "Unix Makefiles" -DDO_TESTING=ON /path/to/code
    make
    # To list the tests
    ctest -N
    # To run all the tests (their names start with
    # the letters 'self').
    ctest -R self

To turn off linking with or using zlib or zstd libraries
or headers there is an option to cmake as of libdwarf 0.9.1:

    cmake -G Ninja -DENABLE_DECOMPRESSION=NO /path/to/code

By default ctest just shows success or failure with no details.
To debug a cmake test, for example if test 22 fails and you
want to know what the test output is, use the following:

    ctest --verbose -I 22

In case one wishes to see the exact compilation/linking options
passed at compile time when using -G "Unix Makefiles":

    make VERBOSE=1

With -G Ninja the generated build.ninja file shows
the build details.
