# Parsing Stack Traces

## Finding the Right Binary

To find the correct binary for a specific log you need to:

1.  Get the commit from the header of the logs.
2.  Use git to locate that commit and check for the surrounding "version bump" commit.
3.  Download and open the binary:

```
curl -O http://s3.amazonaws.com/downloads.mongodb.org/linux/mongodb-linux-x86_64-debugsymbols-1.x.x.tgz
```

You can also get the debugsymbols archive for official builds through [the Downloads page][1]. In the
Archived Releases section, click on the appropriate platform link to view the available archives.
Select the appropriate debug symbols archive.

## Using mongosymb.py to get file and line numbers

Stacktraces are logged on a line with `msg` `BACKTRACE`. The full backtrace contents are available in
an attribute named `bt`. To convert this into a list of source locations with file and line numbers,
copy the contents of the `bt` JSON blob into a file, then direct the contents of that file into
the standard input of `buildscripts/mongosymb.py`:

```
cat bt | buildscripts/mongosymb.py --debug-file-resolver=path path/to/debug/symbols/file
```

### Example

Given these contents of `bt`:

```
{"bt":{"backtrace":[{"a":"C7FB141ACA08","b":"C7FAFAD30000","o":"1947CA08","s":"_ZN5mongo18stack_trace_detail12_GLOBAL__N_117getStackTraceImplERKNS1_7OptionsE","C":"mongo::stack_trace_detail::(anonymous namespace)::getStackTraceImpl(mongo::stack_trace_detail::(anonymous namespace)::Options const&)","s+":"5C"},{"a":"C7FB141ACC98","b":"C7FAFAD30000","o":"1947CC98","s":"_ZN5mongo15printStackTraceEv","C":"mongo::printStackTrace()","s+":"44"},{"a":"C7FB1417B8A8","b":"C7FAFAD30000","o":"1944B8A8","s":"_ZN5mongo12_GLOBAL__N_126printStackTraceNoRecursionEv","C":"mongo::(anonymous namespace)::printStackTraceNoRecursion()","s+":"38"},{"a":"C7FB1417AEAC","b":"C7FAFAD30000","o":"1944AEAC","s":"_ZN5mongo12_GLOBAL__N_115printErrorBlockEv","C":"mongo::(anonymous namespace)::printErrorBlock()","s+":"C"},{"a":"C7FB1417B1FC","b":"C7FAFAD30000","o":"1944B1FC","s":"abruptQuitWithAddrSignal","s+":"118"},{"a":"E4B2A019B9D0","b":"E4B2A019B000","o":"9D0","s":"__kernel_rt_sigreturn","s+":"0"},{"a":"C7FB0A37ADD0","b":"C7FAFAD30000","o":"F64ADD0","s":"_ZN5mongo12_GLOBAL__N_114_initAndListenEPNS_14ServiceContextE","C":"mongo::(anonymous namespace)::_initAndListen(mongo::ServiceContext*)","s+":"AC"},{"a":"C7FB0A3785CC","b":"C7FAFAD30000","o":"F6485CC","s":"_ZN5mongo12_GLOBAL__N_113initAndListenEPNS_14ServiceContextE","C":"mongo::(anonymous namespace)::initAndListen(mongo::ServiceContext*)","s+":"2C"},{"a":"C7FB0A373ED4","b":"C7FAFAD30000","o":"F643ED4","s":"_ZN5mongo11mongod_mainEiPPc","C":"mongo::mongod_main(int, char**)","s+":"7C8"},{"a":"C7FB0A364448","b":"C7FAFAD30000","o":"F634448","s":"main","s+":"24"},{"a":"E4B29F8573FC","b":"E4B29F830000","o":"273FC","s":"__libc_start_call_main","s+":"6C"},{"a":"E4B29F8574CC","b":"E4B29F830000","o":"274CC","s":"__libc_start_main_alias_2","s+":"98"}],"processInfo":{ <redacted> }}}
```

The following command might produce the following output:

```
$ cat bt | buildscripts/mongosymb.py --debug-file-resolver=path bazel-bin/install-mongod/bin/mongod
...
 /proc/self/cwd/src/mongo/util/stacktrace_posix.cpp:428:19: mongo::stack_trace_detail::(anonymous namespace)::getStackTraceImpl(mongo::stack_trace_detail::(anonymous namespace)::Options const&)
 /proc/self/cwd/src/mongo/util/stacktrace_posix.cpp:480:5: mongo::printStackTrace()
 /proc/self/cwd/src/mongo/util/signal_handlers_synchronous.cpp:204:9: mongo::(anonymous namespace)::printStackTraceNoRecursion()
 /proc/self/cwd/src/mongo/util/signal_handlers_synchronous.cpp:232:5: mongo::(anonymous namespace)::printErrorBlock()
 /proc/self/cwd/src/mongo/util/signal_handlers_synchronous.cpp:321:5: abruptQuitWithAddrSignal
 ??:0:0: ??
 /proc/self/cwd/src/mongo/db/mongod_main.cpp:527:48: mongo::(anonymous namespace)::_initAndListen(mongo::ServiceContext*)
 /proc/self/cwd/src/mongo/db/mongod_main.cpp:1194:16: mongo::(anonymous namespace)::initAndListen(mongo::ServiceContext*)
 /proc/self/cwd/src/mongo/db/mongod_main.cpp:2117:25: mongo::mongod_main(int, char**)
 /proc/self/cwd/src/mongo/db/mongod.cpp:45:22: main
...
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

[1]: https://www.mongodb.com/download-center
