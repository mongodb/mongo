# Instructions for Updating KLZ4 Version

This document describes the steps to update the bundled lz4 version, that is,
the version used when `./configure` is run with `--disable-lz4-ext`.

1. For each file in the [lz4 repository's](https://github.com/lz4/lz4/) `lib`
   directory (checked out to the appropriate version tag), copy it into the
   librdkafka `src` directory, overwriting the previous files.
2. Copy `xxhash.h` and `xxhash.c` files, and rename them to `rdxxhash.h` and
   `rdxxhash.c`, respectively, replacing the previous files. Change any
   `#include`s of `xxhash.h` to `rdxxhash.h`.
3. Replace the `#else` block of the
   `#if defined(KLZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION)`
   with the following code, including the comment:
   ```c
    #else
    /* NOTE: While upgrading the lz4 version, replace the original `#else` block
    * in the code with this block, and retain this comment. */
    struct rdkafka_s;
    extern void *rd_kafka_mem_malloc(struct rdkafka_s *rk, size_t s);
    extern void *rd_kafka_mem_calloc(struct rdkafka_s *rk, size_t n, size_t s);
    extern void rd_kafka_mem_free(struct rdkafka_s *rk, void *p);
    # define ALLOC(s)          rd_kafka_mem_malloc(NULL, s)
    # define ALLOC_AND_ZERO(s) rd_kafka_mem_calloc(NULL, 1, s)
    # define FREEMEM(p)        rd_kafka_mem_free(NULL, p)
    #endif
    ```
4. Change version mentioned for lz4 in `configure.self`.
4. Run `./configure` with `--disable-lz4-ext` option, make and run test 0017.
5. Update CHANGELOG.md and both the lz4 LICENSE, and the combined LICENSE.
