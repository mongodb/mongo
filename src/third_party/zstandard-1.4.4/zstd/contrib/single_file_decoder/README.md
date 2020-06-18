# Single File Zstandard Decompression Library

The script `combine.sh` creates an _amalgamated_ source file that can be used with or without `zstd.h`. This isn't a _header-only_ file but it does offer a similar level of simplicity when integrating into a project.

Create `zstddeclib.c` from the Zstd source using:
```
cd zstd/contrib/declib
./combine.sh -r ../../lib -r ../../lib/common -r ../../lib/decompress -o zstddeclib.c zstddeclib-in.c
```
Then add the resulting file to your project (see the [example files](examples)).

`create_single_file_decoder.sh` will run the above script, creating file `zstddeclib.c`.
`build_test.sh` will create the decoder, then compile and test the library.

Why?
----

Because all it now takes to support decompressing Zstd is the addition of a single file, two if using the header, with no configuration or further build steps. The library is small, adding, for example, 26kB to an Emscripten compiled WebAssembly project. Native implementations add a little more, 40-70kB depending on the compiler and platform.
