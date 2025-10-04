Zstandard Documentation
=======================

This directory contains material defining the Zstandard format,
as well as detailed instructions to use `zstd` library.

__`zstd_manual.html`__ : Documentation of `zstd.h` API, in html format.
Unfortunately, Github doesn't display `html` files in parsed format, just as source code.
For a readable display of html API documentation of latest release,
use this link: [https://raw.githack.com/facebook/zstd/release/doc/zstd_manual.html](https://raw.githack.com/facebook/zstd/release/doc/zstd_manual.html) .

__`zstd_compression_format.md`__ : This document defines the Zstandard compression format.
Compliant decoders must adhere to this document,
and compliant encoders must generate data that follows it.

Should you look for resources to develop your own port of Zstandard algorithm,
you may find the following resources useful :

__`educational_decoder`__ : This directory contains an implementation of a Zstandard decoder,
compliant with the Zstandard compression format.
It can be used, for example, to better understand the format,
or as the basis for a separate implementation of Zstandard decoder.

[__`decode_corpus`__](https://github.com/facebook/zstd/tree/dev/tests#decodecorpus---tool-to-generate-zstandard-frames-for-decoder-testing) :
This tool, stored in `/tests` directory, is able to generate random valid frames,
which is useful if you wish to test your decoder and verify it fully supports the specification.
