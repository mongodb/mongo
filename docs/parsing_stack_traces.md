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

## Stack Trace Schema

Stack traces are typically logged as log message 31380, having a `bt` attribute
that holds a JSON object value:

```json
"bt": {
    "backtrace": [ # Array of frame objects.
        {
            "a" : (hex string) instruction address
            "b" : (hex string) base of its section (library)
            "o" : (hex string) offset within that section (i.e., o = a - b)
            "s" : symbol name
            "C" : C++ demangling of `s` (if available)
            "s+": (hex string) offset of `a` within symbol `s`.
                  So `s` and `s+` elements taken together are analogous to gdb's `functionName+hexOffset`
                  representation.
        }, ...
    ],
    "processInfo": {
        ...
        "somap": [ # Array of shared object metadata objects.
          {
            "b": (hex string) dynamic base address for the library
            "path": filename of the library
            ...
          },
          ...
        ]
    }
}
```

The "processInfo" subobject has other information about the process, but
the most important thing for the stack trace is the "somap", which is an
array of all dynamically linked ELF files, including the main executable,
and where in memory they were loaded.

Partial example showing a few typical frames:

```json
{
  "t": {"$date": "2025-05-20T19:50:28.402+00:00"},
  "s": "I",
  "c": "CONTROL",
  "id": 31380,
  "ctx": "main",
  "msg": "BACKTRACE",
  "attr": {
    "bt": {
      "backtrace": [
        ...
        {
          "a": "BBEB14AB4D38",
          "b": "BBEAFBBE0000",
          "o": "18ED4D38",
          "s": "_ZN5boost15program_options25basic_command_line_parserIcE3runEv",
          "C": "boost::program_options::basic_command_line_parser<char>::run()",
          "s+": "58"
        },
        {
          "a": "BBEB0B43AF08",
          "b": "BBEAFBBE0000",
          "o": "F85AF08",
          "s": "_ZN5mongo17optionenvironment12_GLOBAL__N_146_mongoInitializerFunction_StartupOptions_ParseEPNS_18InitializerContextE",
          "C": "mongo::optionenvironment::(anonymous namespace)::_mongoInitializerFunction_StartupOptions_Parse(mongo::InitializerContext*)",
          "s+": "58"
        },
        ...
        {
          "a": "BBEB0B030708",
          "b": "BBEAFBBE0000",
          "o": "F450708",
          "s": "main",
          "s+": "24"
        },
        ...
      ],
      "processInfo": {
        ...
        "somap": [
          {
            "b": "BBEAFBBE0000",
            "path": "/home/ubuntu/.../mongod_with_debug",
            "buildId": "0968C21BDD5AE9ADAC4CD6854023103AECCC1907",
            "elfType": 3
          },
          {
            "b": "E366E4620000",
            "path": "/lib/aarch64-linux-gnu/libc.so.6",
            "elfType": 3,
            "buildId": "2A450FE74D1B79A321CC1B12337FC31A2C3FB834"
          }
        ]
      }
    }
  },
  "tags": []
}
```

[1]: https://sourceware.org/binutils/docs/binutils/addr2line.html
[2]: https://sourceware.org/binutils/docs-2.17/binutils/c_002b_002bfilt.html
[3]: https://www.mongodb.com/download-center
