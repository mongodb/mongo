zstdless(1) -- view zstandard-compressed files
============================================================================

SYNOPSIS
--------

`zstdless` [<flags>] [<file> ...]


DESCRIPTION
-----------
`zstdless` runs `less`(1) on files or stdin, if no <file> argument is given, after decompressing them with `zstdcat`(1).

SEE ALSO
--------
`zstd`(1)
