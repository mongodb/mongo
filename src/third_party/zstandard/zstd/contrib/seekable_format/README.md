# Zstandard Seekable Format

The seekable format splits compressed data into a series of independent "frames",
each compressed individually,
so that decompression of a section in the middle of an archive
only requires zstd to decompress at most a frame's worth of extra data,
instead of the entire archive.

The frames are appended, so that the decompression of the entire payload
still regenerates the original content, using any compliant zstd decoder.

On top of that, the seekable format generates a jump table,
which makes it possible to jump directly to the position of the relevant frame
when requesting only a segment of the data.
The jump table is simply ignored by zstd decoders unaware of the seekable format.

The format is delivered with an API to create seekable archives
and to retrieve arbitrary segments inside the archive.

### Maximum Frame Size parameter

When creating a seekable archive, the main parameter is the maximum frame size.

At compression time, user can manually select the boundaries between segments,
but they don't have to: long segments will be automatically split
when larger than selected maximum frame size.

Small frame sizes reduce decompression cost when requesting small segments,
because the decoder will nonetheless have to decompress an entire frame
to recover just a single byte from it.

A good rule of thumb is to select a maximum frame size roughly equivalent
to the access pattern when it's known.
For example, if the application tends to request 4KB blocks,
then it's a good idea to set a maximum frame size in the vicinity of 4 KB.

But small frame sizes also reduce compression ratio,
and increase the cost for the jump table,
so there is a balance to find.

In general, try to avoid really tiny frame sizes (<1 KB),
which would have a large negative impact on compression ratio.
