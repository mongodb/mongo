Zstandard library files
================================

The __lib__ directory is split into several sub-directories,
in order to make it easier to select or exclude features.


#### Building

`Makefile` script is provided, supporting all standard [Makefile conventions](https://www.gnu.org/prep/standards/html_node/Makefile-Conventions.html#Makefile-Conventions),
including commands variables, staged install, directory variables and standard targets.
- `make` : generates both static and dynamic libraries
- `make install` : install libraries in default system directories

`libzstd` default scope includes compression, decompression, dictionary building,
and decoding support for legacy formats >= v0.5.0.


#### API

Zstandard's stable API is exposed within [lib/zstd.h](zstd.h).


#### Advanced API

Optional advanced features are exposed via :

- `lib/common/zstd_errors.h` : translates `size_t` function results
                              into an `ZSTD_ErrorCode`, for accurate error handling.
- `ZSTD_STATIC_LINKING_ONLY` : if this macro is defined _before_ including `zstd.h`,
                          it unlocks access to advanced experimental API,
                          exposed in second part of `zstd.h`.
                          These APIs are not "stable", their definition may change in the future.
                          As a consequence, it shall ___never be used with dynamic library___ !
                          Only static linking is allowed.


#### Modular build

It's possible to compile only a limited set of features.

- Directory `lib/common` is always required, for all variants.
- Compression source code lies in `lib/compress`
- Decompression source code lies in `lib/decompress`
- It's possible to include only `compress` or only `decompress`, they don't depend on each other.
- `lib/dictBuilder` : makes it possible to generate dictionaries from a set of samples.
        The API is exposed in `lib/dictBuilder/zdict.h`.
        This module depends on both `lib/common` and `lib/compress` .
- `lib/legacy` : source code to decompress legacy zstd formats, starting from `v0.1.0`.
        This module depends on `lib/common` and `lib/decompress`.
        To enable this feature, define `ZSTD_LEGACY_SUPPORT` during compilation.
        Specifying a number limits versions supported to that version onward.
        For example, `ZSTD_LEGACY_SUPPORT=2` means : "support legacy formats >= v0.2.0".
        `ZSTD_LEGACY_SUPPORT=3` means : "support legacy formats >= v0.3.0", and so on.
        Currently, the default library setting is `ZST_LEGACY_SUPPORT=5`.
        It can be changed at build by any other value.
        Note that any number >= 8 translates into "do __not__ support legacy formats",
        since all versions of `zstd` >= v0.8 are compatible with v1+ specification.
        `ZSTD_LEGACY_SUPPORT=0` also means "do __not__ support legacy formats".
        Once enabled, this capability is transparently triggered within decompression functions.
        It's also possible to invoke directly legacy API, as exposed in `lib/legacy/zstd_legacy.h`.
        Each version also provides an additional dedicated set of advanced API.
        For example, advanced API for version `v0.4` is exposed in `lib/legacy/zstd_v04.h` .
        Note : `lib/legacy` only supports _decoding_ legacy formats.
- Similarly, you can define `ZSTD_LIB_COMPRESSION, ZSTD_LIB_DECOMPRESSION`, `ZSTD_LIB_DICTBUILDER`,
        and `ZSTD_LIB_DEPRECATED` as 0 to forgo compilation of the corresponding features. This will
        also disable compilation of all dependencies (eg. `ZSTD_LIB_COMPRESSION=0` will also disable
        dictBuilder).


#### Multithreading support

Multithreading is disabled by default when building with `make`.
Enabling multithreading requires 2 conditions :
- set macro `ZSTD_MULTITHREAD`
- on POSIX systems : compile with pthread (`-pthread` compilation flag for `gcc`)

Both conditions are automatically triggered by invoking `make lib-mt` target.
Note that, when linking a POSIX program with a multithreaded version of `libzstd`,
it's necessary to trigger `-pthread` flag during link stage.

Multithreading capabilities are exposed
via [advanced API `ZSTD_compress_generic()` defined in `lib/zstd.h`](https://github.com/facebook/zstd/blob/dev/lib/zstd.h#L919).
This API is still considered experimental,
but is expected to become "stable" at some point in the future.


#### Windows : using MinGW+MSYS to create DLL

DLL can be created using MinGW+MSYS with the `make libzstd` command.
This command creates `dll\libzstd.dll` and the import library `dll\libzstd.lib`.
The import library is only required with Visual C++.
The header file `zstd.h` and the dynamic library `dll\libzstd.dll` are required to
compile a project using gcc/MinGW.
The dynamic library has to be added to linking options.
It means that if a project that uses ZSTD consists of a single `test-dll.c`
file it should be linked with `dll\libzstd.dll`. For example:
```
    gcc $(CFLAGS) -Iinclude/ test-dll.c -o test-dll dll\libzstd.dll
```
The compiled executable will require ZSTD DLL which is available at `dll\libzstd.dll`.


#### Deprecated API

Obsolete API on their way out are stored in directory `lib/deprecated`.
At this stage, it contains older streaming prototypes, in `lib/deprecated/zbuff.h`.
These prototypes will be removed in some future version.
Consider migrating code towards supported streaming API exposed in `zstd.h`.


#### Miscellaneous

The other files are not source code. There are :

 - `LICENSE` : contains the BSD license text
 - `Makefile` : `make` script to build and install zstd library (static and dynamic)
 - `BUCK` : support for `buck` build system (https://buckbuild.com/)
 - `libzstd.pc.in` : for `pkg-config` (used in `make install`)
 - `README.md` : this file
