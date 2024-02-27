# Parsing Stack Traces

## `addr2line`

[`addr2line`][1] is a utility to translate addresses into filenames and line numbers.

```
addr2line -e mongod -ifC <offset>
```

## `c++filt`

Use [`c++filt`][2] to demangle function names by pasting the whole stack trace to stdin.

## Finding the Right Binary

To find the correct binary for a specific log you need to:

1.  Get the commit from the header of the logs.
2.  Use git to locate that commit and check for the surrounding "version bump" commit.
3.  Download and open the binary:

```
curl -O http://s3.amazonaws.com/downloads.mongodb.org/linux/mongodb-linux-x86_64-debugsymbols-1.x.x.tgz
```

You can also get the debugsymbols archive for official builds through [the Downloads page][3]. In the
Archived Releases section, click on the appropriate platform link to view the available archives.
Select the appropriate debug symbols archive.

### Example: Reading the Log

Note that the log has lines like this:

```
/home/abc/mongod(_ZN5mongo15printStackTraceERSo+0x27) [0x689280]
```

You want to use the address in between the brackets `0x689280`. Note that you will get more than one
stack frame for the address if the code is inlined.

### Example: Using `addr2line`

Actual example from a v1.8.1 64-bit Linux build:

```
$ curl http://downloads.mongodb.org/linux/mongodb-linux-x86_64-debugsymbols-1.8.1.tgz > out.tgz
$ tar -xzf out.tgz
$ cd mongodb-linux-x86_64-debugsymbols-1.8.1/
$ cd bin
$ addr2line --help
$ addr2line -i -e mongod 0x6d6a74
/mnt/home/buildbot/slave/Linux_64bit_V1.8/mongo/db/repl/health.cpp:394
$ addr2line -i -e mongod 0x6d0694
/mnt/home/buildbot/slave/Linux_64bit_V1.8/mongo/db/repl/rs.h:385
/mnt/home/buildbot/slave/Linux_64bit_V1.8/mongo/db/repl/replset_commands.cpp:111
```

[1]: https://sourceware.org/binutils/docs/binutils/addr2line.html
[2]: https://sourceware.org/binutils/docs-2.17/binutils/c_002b_002bfilt.html
[3]: https://www.mongodb.com/download-center
