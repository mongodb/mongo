# Single File Zstandard Libraries

The script `combine.sh` creates an _amalgamated_ source file that can be used with or without `zstd.h`. This isn't a _header-only_ file but it does offer a similar level of simplicity when integrating into a project.

All it now takes to support Zstd in your own projects is the addition of a single file, two if using the header, with no configuration or further build steps.

Decompressor
------------

This is the most common use case. The decompression library is small, adding, for example, 26kB to an Emscripten compiled WebAssembly project. Native implementations add a little more, 40-70kB depending on the compiler and platform.

Create `zstddeclib.c` from the Zstd source using:
```
cd zstd/build/single_file_libs
./combine.sh -r ../../lib -o zstddeclib.c zstddeclib-in.c
```
Then add the resulting file to your project (see the [example files](examples)).

`create_single_file_decoder.sh` will run the above script, creating the file `zstddeclib.c` (`build_decoder_test.sh` will also create `zstddeclib.c`, then compile and test the result).

Full Library
------------

The same tool can amalgamate the entire Zstd library for ease of adding both compression and decompression to a project. The [roundtrip example](examples/roundtrip.c) uses the original `zstd.h` with the remaining source files combined into `zstd.c` (currently just over 1.2MB) created from `zstd-in.c`. As with the standalone decoder the most useful compile flags have already been rolled-in and the resulting file can be added to a project as-is.

Create `zstd.c` from the Zstd source using:
```
cd zstd/build/single_file_libs
./combine.sh -r ../../lib -o zstd.c zstd-in.c
```
It's possible to create a compressor-only library but since the decompressor is so small in comparison this doesn't bring much of a gain (but for the curious, simply remove the files in the _decompress_ section at the end of `zstd-in.c`).

`create_single_file_library.sh` will run the script to create `zstd.c` (`build_library_test.sh` will also create `zstd.c`, then compile and test the result).
