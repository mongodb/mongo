crc32-s390x
===========

A library of functions for accelerating CRC32 calculations using the
Vector Galois Field Multiply instruction family instructions introduced in
the z13. The Vector Extension Facility for z/Architecture provides two
instructions to perform a binary Galois field multiplication. Both
instructions (VGFM and VGFMA) can operate on different element sizes.
For the 32-bit CRC computation, doublewords are used throughout the
implementation.

Quick start
-----------

Type make to generate a static library libcrc32\_s390x.a.

The library provides functions to compute CRC-32 (IEEE 802.3) and
CRC-32C (Castagnoli), with optional bit reflection (with the `*_le`
versions of the functions).

Function prototypes are declared in crc32-s390x.h. A sample program
crc32-cli.c shows how the library is used.

Testing
-------

The correctness of the hardware-accelerated implementation is verified
with the pure-software Rocksoft Model CRC algorithm. There are four
variants of the test, each of which exercise one type of CRC on random
data with random alignment and buffer sizes, in an infinite loop:

    ./crc32_be_test
    ./crc32_le_test
    ./crc32c_be_test
    ./crc32c_le_test

If the hardware-accelerated algorithm ever returns a different result
than the Rocksoft Model, the test will print messages to indicate the
errors.

Performance
-----------

The performance of the hardware-accelerated implemention is compared
with the slicing-by-8 algorithm. Testing 500000 iterations of a CRC
of 32kB of data showed a 70-times speed-up:

    $ time ./crc32_sw_bench 32768 500000
    CRC: a98177aa
    
    real    0m21.862s
    user    0m21.859s
    sys     0m0.002s
    
    $ time ./crc32_vx_bench 32768 500000
    CRC: a98177aa
    
    real    0m0.323s
    user    0m0.323s
    sys     0m0.000s

