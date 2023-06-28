// The Antithesis Instrumentation Library has three main uses:
//
// 1. Enabling the system under test, or its test harness, to request input from the Fuzzer.
// 2. Enabling the system under test, or its test harness, to send arbitrary messages, guidance, or information
//    about current status back to the Fuzzer. This information can either be used by the Fuzzer to make further
//    decisions (e.g. in strategies), or logged to BigQuery for post-hoc analysis.
// 3. Providing the callbacks which programs that have been *instrumented* use to send coverage data back to 
//    the Fuzzer.
//
//
// Purpose (1) is "input". Purposes (2) and (3) are conceptually both forms of "output", but are treated separately
// because in practice they are used very differently. Output of type (2) is more commonly associated with test
// harnesses and wrappers, though perhaps someday our customers will add it to their programs directly using a 
// descendant of this library. Output of type (3) is generally performed by the target program directly. It does this 
// if it has been transformed ("instrumented") by an "instrumentor", sometimes at compile-time, sometimes afterwards, but 
// in either case conceptually part of the build. This process of "instrumentation" involves adding callbacks to all or 
// most of the program edges in the customer program, such that every time the edge is reached, the callback is executed,
// sending coverage information back to the Fuzzer.
//
// This means that in some sense, the word "instrumentation" itself is overloaded, as it can refer both to the general
// category of I/O handled by this library, and to the specific kinds grouped under type (3) above. Sorry about that.
//
// There are three distinct implementations of this interface:
//
// * libvoidstar.so
// * libinstrumentation_legacy.so
// * libinstrumentation_determ.so
// 
// libvoidstar.so is the stub implementation of this library. Its output functions either do nothing or print to the
// console. Its input functions are a shim to the real `getchar`. The primary purpose of this stub is to be 
// distributed to customers, so that they can link it as part of their build. When it comes time to actually test
// the customer's software, we then use LD_PRELOAD or similar to substitute one of the two "real" instrumentation 
// libraries in its place. A nice benefit of this is that we can change all kinds of details about the real 
// implementations without requiring our customers to re-compile their software, so long as we don't modify the interface.
//
// libinstrumentation_legacy.so is the "real" implementation for tests running outside of our deterministic hypervisor. It
// communicates with the Fuzzer over local Unix sockets, using a protocol that is decoded on the Fuzzer side by the
// `LegacyTarget` class. In practice this is used relatively rarely these days, since it is only useful for 
// fuzzing targets that are naturally deterministic. Consequently, many of the methods have various limitations
// in this version of the library, or in a few cases are just stubs. Still, it can be convenient to have for local 
// testing.
//
// libinstrumentation_determ.so is the "real" implementation for tests running inside of our deterministic hypervisor. 
// It communicates with the Fuzzer using the libsyncio library, which in turn performs a hypercall that causes the 
// hypervisor to halt execution of the running VM and exchange data with the Fuzzer (see the libsyncio documentation for 
// details). Note that because of this, it is *not possible* to run with this library outside of the hypervisor -- your
// CPU will consider the hypercall to be an invalid instruction, and you will get a SIGILL.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// *********************************************
// (1) Input functions
// *********************************************

// Request an additional byte of input from the Fuzzer. This will lead to the creation and logging of a new `State`.
// Indeed, you could actually define a State to be the duration of system time between successive calls to 
// `fuzz_getchar`.
//
// This method is thread-safe. It's fine for multiple systems/test harnesses to call this function, though that may
// in some cases make it more challenging to write intelligent strategies and tactics in the Fuzzer.
//
// This method is not only synchronous, but in some sense "globally" synchronous. Tests using libinstrumentation_legacy.so 
// are presumed to be naturally deterministic, hence single-threaded and single-process. Meanwhile, the 
// libinstrumentation_determ.so version of this function will, due to the way that libsyncio works, freeze the entire 
// guest system until a reply is received from the Fuzzer (obviously a context switch could still occur between 
// returning from the syncio library and returning from `fuzz_getchar`).
//
// Before anything happens, this will also trigger a flush of any buffered output messages sent via this copy of the 
// instrumentation library (equivalent to calling `fuzz_flush` as described below).
int fuzz_getchar();

// Returns a pseudorandom number that is a deterministic function of the sequence of prior calls to `fuzz_getchar` by this 
// or *other* copies of the instrumentation library, the sequence of prior random number accesses, and of nothing else.
//
// This method is thread-safe in libinstrumentation_determ (the legacy instrumentation should never be used with
// multi-threaded programs).
uint64_t fuzz_get_random();

// Returns a boolean that is a deterministic function of the sequence of prior calls to `fuzz_getchar` by this 
// or *other* copies of the instrumentation library, the sequence of prior random number accesses, and of nothing else.
//
// This method is thread-safe in libinstrumentation_determ (the legacy instrumentation should never be used with
// multi-threaded programs).
bool fuzz_coin_flip();

// A rarely-used function that requests an entire large blob of input from the Fuzzer. This is NEVER called during 
// normal testing operations, and is primarily used during the test setup phase as a means of optionally injecting
// data or configuration into the guest system. We have moved away from using this method of injecting things into the
// guest system, though, and recommend that that occur as part of the LiveCD build instead.
//
// The libinstrumentation_legacy.so version of this function is just a shim to `fuzz_getchar` and only ever returns one byte.
//
// This method is thread-safe.
//
// TODO: Decide whether to remove this method from the public instrumentation interface.
size_t fuzz_getblob(void* buffer, size_t buffer_size);

// *********************************************
// (2) Output functions (non-coverage)
// *********************************************

// Sets a name that will be associated with all future output messages *and* coverage output coming from this copy of 
// the instrumentation library. Useful for disambiguation when multiple harnesses/programs are concurrently sending 
// output and/or coverage to the Fuzzer. The name is plumbed all the way through to Notebook for analysis.
//
// This method only needs to be called once per instantiation of this library, and the configured source name will
// persist until this method is called again. It does not need to be called once per thread. Currently multiple
// threads in a program that has loaded this library *must* share a source name.
//
// This method is implicitly called by the instrumentor-injected module initializers (see discussion below). Calling it 
// again in an instrumented program will override their naming scheme.
//
// This method is thread-safe.
//
// The maximum length for a source name is 64 characters.
void fuzz_set_source_name( const char* name );

// This family of methods is the primary means of sending unstructured data from the system under test and the test
// harnesses back to the Fuzzer. Examples of things that are commonly sent with these methods include log messages,
// exceptions or error messages, program text output, and even "hand" instrumentation information.
//
// If the Fuzzer is configured to log program output, these messages will be available for analysis in the Notebook.
// Every message will be associated with: (1) the state in which it was generated, (2) the source name that produced 
// the message (if `fuzz_set_source_name` has been called), (3) whether the message was `ERROR` or `INFO`, and (4) the 
// VTime at which the message was generated (if running in the hypervisor). Sorting, filtering, grouping, etc. on these 
// metadata fields is relatively straightforward in the Notebook.
//
// Strategies may also make use of these messages as a form of guidance. See, e.g. `MessageFeature` and
// `MessageMatchRegexFeature` in the Fuzzer.
//
// The "message" versions of these functions expect a null-terminated C-style string, the "data" versions expect a 
// pointer and a length. They are otherwise identical.
//
// These methods are thread-safe.
// 
// WARNING: These methods buffer output data to be sent to the Fuzzer in batches. They do *not* immediately send
// data when called. It is guaranteed that output messages will never be buffered across state boundaries **WITHIN
// A SINGLE COPY OF THE INSTRUMENTATION LIBRARY**, however if a guest vm contains multiple output sources, it is 
// completely possible for their outputs to be almost arbitrarily interleaved, *including* across state boundaries.
// Moreover the VTime that is eventually attributed to a message is the VTime at which it was sent, not the VTime at 
// which this function was called and the message was buffered.
//
// If you want a message to be sent immediately, you should call `fuzz_flush` after buffering the message.
void fuzz_info_message( const char* message );
void fuzz_info_data( const char* message, size_t length );
void fuzz_error_message( const char* message );
void fuzz_error_data( const char* message, size_t length );

// Slight variation on `fuzz_info_data` which assumes that the data is PNG-encoded, and informs the Fuzzer of that
// fact. Primarily used for sending screenshots of the system under test for analysis or visualization.
//
// All guarantees/invariants/warnings listed for `fuzz_info_message` are also true for this method.
void fuzz_png( const void* png, size_t length );

// Variation on fuzz_info_data and fuzz_error_data for any blob of binary data.
void fuzz_bytes ( const void* bytes, size_t length);

// Variation on `fuzz_info_data` which instructs the Fuzzer to interpret the payload as an array of uint32 key-value
// pairs, packed like: 'KVKVKV...'. The data can be unpacked on the Fuzzer side using a `KV32ValueFeature`, which can
// in turn be logged or used in a strategy like any other feature.
//
// It is NOT recommended that you call this multiple times within a single state with the same Key, unless you also do so
// with the same Value every time. A `KV32ValueFeature` evaluated on a state where the same Key has been logged 
// multiple times will pick *one* of the possible Values, but you should not make any assumptions about which one!
//
// `length` is the total length of the array passed in, twice the number of features.
//
// All guarantees/invariants/warnings listed for `fuzz_info_message` are also true for this method.
void fuzz_kv32_pairs(const uint32_t *pair_arr, size_t length);

// Causes all previously buffered output messages in this copy of the library to be sent immediately, and does not return
// until a response is received. From the perspective of the system under test, this happens "almost instantaneously". 
// but from the point of view of real world performance, this triggers a network roundtrip to and from the Fuzzer, which 
// may involve significant latency.
//
// This method is thread-safe.
void fuzz_flush();

// Sends a termination message to the Fuzzer with the provided exit code, then causes this process to exit without cleanup.
// This output message is not buffered, but will be sent *immediately* (no need to call `fuzz_flush`). The state in which 
// this message was sent will be marked as Terminal by the Fuzzer, meaning no descendants of the state will ever be 
// produced. Note however that some Fuzzer strategies may still choose to remember Terminal states, for instance in order
// to apply `BackupTactic` or `MutationTactic` to them.
//
// Exit codes are logged, and available for analysis in the Notebook.
//
// This method is thread-safe.
void fuzz_exit(int exit_code);

// *********************************************
// (3) Output functions (coverage callbacks)
// *********************************************

// Programs using LLVM instrumentation have this callback inserted by the compiler as a module constructor into every DSO.
// Developers generally should not have to manually call this function. It is thread-safe and may be called multiple 
// times with the same parameters.
//
// Our implementation of the LLVM module initializer callback does 3 things:
// 
// (1) Initializes all edge guards to 1 (see documentation for `__sanitizer_cov_trace_pc_guard`).
// (2) Finds the PID of this process and reads the value of the `ANT_SOURCE_NAME` environment variable. If the variable is
//     non-empty, sets the source name for this copy of the library to "${ANT_SOURCE_NAME}-${PID}", if it is empty, sets
//     it to "instrumentation-${PID}". See documentation for `fuzz_set_source_name` for more on what that means.
// (3) Sends to the Fuzzer information about all of the memory segments in which this module has code loaded. This 
//     information is used by the Fuzzer in the symbolization process.
//
// The module information is not buffered, but sent immediately.
//
// The following symbols are indeed reserved identifiers, since we're implementing functions defined
// in the compiler runtime. Not clear how to get Clang on board with that besides narrowly suppressing
// the warning in this case. The sample code on the CoverageSanitizer documentation page fails this 
// warning!
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-identifier"
void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop);

// Programs using LLVM instrumentation have this callback inserted by the compiler on every edge in the control flow (some
// optimizations apply). Developers generally should not have to manually call this function. It is thread-safe and may be 
// called multiple times with the same parameters. Typically, the compiler will emit the code like this:
//
//    if(*guard)
//      __sanitizer_cov_trace_pc_guard(guard);
//
// But for large functions it will emit a simple call:
//
//    __sanitizer_cov_trace_pc_guard(guard);
//
// Thus, by setting *guard to 0 the function will *usually* not be called again for a given edge.
//
// Our implementation of the LLVM edge callback buffers a message to the Fuzzer containing the address of the instruction
// that triggered the callback. The first time the callback is triggered for a given edge, the coverage message is 
// buffered unconditionally. Sucessive invocations have their behavior governed by the `ANT_DISABLE_REUSED_EDGE` 
// environment variable. If the variable is unset, or set to "1", the callback will only send coverage information the
// first time. If set to anything other than "1", the callback will send coverage information every time.
//
// Setting `ANT_DISABLE_REUSED_EDGE` to something other than 1 is necessary for the functioning of certain Fuzzer
// strategies (for example TransitionCoverageStrategy), however the volume of edge data that results currently causes a 
// massive performance overhead, so it is not enabled by default, and not practical for many fuzzing targets.
//
// There are many features in the Fuzzer's feature algebra that are functions of coverage data, and many strategies that
// make use of them. Coverage information can also be visualized and analyzed in the Notebook.
void __sanitizer_cov_trace_pc_guard(uint32_t *guard);
#pragma clang diagnostic pop

// Each independently instrumented module should call init_coverage_module() once.  The symbol file name will be used
// to look up symbols, and should generally be different for each instrumented module.  The edge_count is the number
// of possible edges in the module.
// The return value is a module identifier which should be added to edge IDs in the range [0,edge_count) and passed
// to notify_coverage().
// This function is thread safe, but it is the responsibility of the caller to call it exactly once for each module,
// before calling notify_coverage().
size_t init_coverage_module(size_t edge_count, const char* symbol_file_name);

// Each time an edge occurs, call notify_coverage( E+M ), where E is the edge ID recorded in the symbol file
// (0 <= E < edge_count) and M is the return value of init_coverage_module(edge_count, ...)
// If the return value is false, the caller *may* omit subsequent calls with the same parameter.  (If it has always been true for that parameter, it must not.)
bool notify_coverage(size_t edge_plus_module);

#ifdef __cplusplus
}
#endif
