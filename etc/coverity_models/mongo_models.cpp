// Coverity model file for MongoDB-specific patterns.
//
// This file is compiled into an xmldb via cov-make-library (see evergreen/coverity_build.sh)
// and placed at <covIdir>/config/user_models.xmldb so cov-analyze picks it up automatically.
//
// Models tried and retired (confirmed ineffective in practice):
//
//   mongo::error_details::invariant*, uasserted*, fassert* — redundant. These functions
//   are already decorated with [[noreturn]] / MONGO_COMPILER_NORETURN, which Coverity
//   understands natively. The models had zero measurable effect on UNINIT/FORWARD_NULL counts.
//
//   mongo::idl::preparsedValue<T> — C++ template models require exact mangled-name matches
//   per concrete instantiation. A single template stub does not generate those mangled names,
//   so the model never matched any call site. UNINIT_CTOR in IDL-generated constructors is
//   instead addressed with inline // coverity[uninit_ctor] annotations in
//   buildscripts/idl/idl/generator.py.
//
//   mongo::Future<T>::then() — auto return type + template parameter makes the mangled
//   name unique per instantiation. Cannot be matched with a single model stub. The
//   related UNINIT_CTOR pattern in future_util.h is handled with inline annotations.
//
// Add new models here when a Coverity FP pattern is identified that:
//   (a) cannot be suppressed with an inline annotation at the source location, AND
//   (b) is driven by a MongoDB-specific function whose semantics Coverity cannot infer.
//
// cov-make-library runs a standalone front-end with no system include paths.
// Use the std::string stub below if signatures require it; do not include standard headers.

// Minimal stubs for types used in function signatures below.
namespace std {
class string {
public:
    string();
    string(const char*);
    ~string();
    const char* c_str() const;
    bool empty() const;
};
}  // namespace std

namespace mongo {

// ---------------------------------------------------------------------------
// Wire-protocol taint — source + sink models
//
// IMPORTANT LIMITATION (discovered through testing, 2026-05):
//   In Coverity's C/C++ analysis, TAINTED_SCALAR tracks two distinct properties:
//     (1) "potentially uninitialized" — used by the UNINIT checker
//     (2) "network-derived/untrusted"  — used by the TAINTED_SCALAR checker
//
//   __coverity_mark_pointee_as_tainted__ sets property (1) only. It is NOT
//   sufficient to make TAINTED_SCALAR fire. TAINTED_SCALAR sources in C/C++
//   are driven exclusively by Coverity's built-in recognition of system-level
//   calls (recv, read, fread, etc.). Adding new TAINTED_SCALAR sources requires
//   Coverity's Security Directives mechanism, not user model files.
//
//   These models are kept because:
//   (a) The SharedBuffer::allocate sink model fires when tainted data from a
//       BUILT-IN Coverity source reaches an allocation without bounds-checking.
//   (b) The CompressionHeader source model increases UNINIT sensitivity on
//       network-derived fields, flagging downstream unvalidated use.
//   (c) The CodeXM checker (etc/coverity_models/codexm/) directly detects the
//       CVE-2025-14847 pattern without relying on TAINTED_SCALAR propagation.
//
// Reference: src/mongo/rpc/op_compressed.h — CompressionHeader(ConstDataRangeCursor*)
// ---------------------------------------------------------------------------

class ConstDataRangeCursor {
public:
    ConstDataRangeCursor();
};

struct CompressionHeader {
    int originalOpCode;
    int uncompressedSize;
    unsigned char compressorId;

    CompressionHeader(ConstDataRangeCursor* cursor) {
        __coverity_mark_pointee_as_tainted__(this, TAINT_TYPE_NETWORK);
    }
};

class SharedBuffer {
public:
    // size_t = unsigned long on x86_64 Linux; matches real allocate(size_t, Allocator={})
    static SharedBuffer allocate(unsigned long bytes, ...) {
        __coverity_taint_sink__(&bytes, TAINTED_SCALAR_GENERIC);
        return SharedBuffer();
    }
};

// ---------------------------------------------------------------------------
// MessageCompressorBase — decompressData / compressData
//
// These pure virtual functions (message_compressor_base.h) account for 12
// RW.ROUTINE_NOT_EMITTED findings. They appear 3 more times in
// message_compressor_manager.cpp. Coverity treats them as opaque black boxes,
// so reads from the output DataRange after a decompress/compress call are
// flagged as UNINIT because Coverity does not know the call wrote to the buffer.
//
// The model marks the output buffer as written (via __coverity_writeall__) so
// that downstream reads from the decompressed/compressed data do not generate
// false-positive UNINIT findings.
//
// Note: DataRange wraps a (char*, size_t) pair. The model stubs provide enough
// structure for the output buffer pointer to be accessible.
//
// References:
//   src/mongo/transport/message_compressor_base.h
//   src/mongo/transport/message_compressor_zlib.cpp   (decompresses wire data)
//   src/mongo/transport/message_compressor_snappy.cpp
//   src/mongo/transport/message_compressor_zstd.cpp
// ---------------------------------------------------------------------------

class ConstDataRange {
public:
    ConstDataRange();
    const char* data() const;
    unsigned long length() const;
};

class DataRange {
public:
    DataRange();
    char* data() const;
    unsigned long length() const;
};

// StatusWith<size_t> stub — only needs to be constructible for return modelling.
class StatusWithSizeT {
public:
    StatusWithSizeT();
    bool isOK() const;
    unsigned long getValue() const;
};

class MessageCompressorBase {
public:
    // Model: decompressData writes compressed-wire-data into the output DataRange.
    // Mark the output buffer as fully written so reads from it after this call
    // do not trigger UNINIT findings in callers (e.g. decompressMessage).
    virtual StatusWithSizeT decompressData(ConstDataRange input, DataRange output) {
        char* buf = output.data();
        __coverity_writeall__(buf);
        StatusWithSizeT result;
        __coverity_writeall__(&result);
        return result;
    }

    // Model: compressData writes the compressed output into the output DataRange.
    virtual StatusWithSizeT compressData(ConstDataRange input, DataRange output) {
        char* buf = output.data();
        __coverity_writeall__(buf);
        StatusWithSizeT result;
        __coverity_writeall__(&result);
        return result;
    }
};

// ---------------------------------------------------------------------------
// OutOfLineExecutor::schedule — service executor callback dispatch
//
// ServiceExecutorAdaptive, ServiceExecutorReserved, and ServiceExecutorSynchronous
// all implement OutOfLineExecutor::schedule(Task func). There are 42
// RW.ROUTINE_NOT_EMITTED findings across service_executor_adaptive.cpp (.h),
// service_executor_reserved.cpp, and service_executor_synchronous.cpp.
//
// When Coverity cannot emit these functions it treats schedule() as a black box:
// values captured by the Task lambda appear to Coverity as never used, generating
// UNINIT findings in callers that create a task, schedule it, then use its output.
//
// The model tells Coverity that schedule() synchronously invokes func with an
// OK-like status. This is a conservative approximation — in reality the call is
// asynchronous — but it is sufficient to propagate initialization/taint state
// through the callback boundary in Coverity's analysis.
//
// Reference: src/mongo/util/out_of_line_executor.h — OutOfLineExecutor::schedule
// ---------------------------------------------------------------------------

class Status {
public:
    Status();
    bool isOK() const;
    static Status OK();
};

// unique_function<void(Status)> stub — the real Task type; only needs
// to be callable with a Status for the model to express the invocation.
class Task {
public:
    Task();
    void operator()(Status s);
};

class OutOfLineExecutor {
public:
    // Model: schedule invokes func with an OK Status, then returns.
    // This tells Coverity that values captured by the lambda ARE used,
    // eliminating false-positive UNINIT findings in callers.
    virtual void schedule(Task func) {
        Status s;
        __coverity_writeall__(&s);
        func(s);
    }
};

// ---------------------------------------------------------------------------
// BSONObj — objdata / getOwned
//
// BSONObjBuilder has 66 RW.ROUTINE_NOT_EMITTED findings. Many arise from template
// specialisations in bsonobjbuilder.h that Coverity cannot emit. The downstream
// effect is that BSONObj values produced by BSONObjBuilder::obj() appear
// uninitialised to Coverity because it cannot trace through the builder's
// template machinery.
//
// These models mark the raw BSON data buffer and the BSONObj itself as fully
// written, eliminating UNINIT findings on fields extracted from BSON objects
// constructed via the builder.
//
// References:
//   src/mongo/bson/bsonobj.h        — BSONObj::objdata()
//   src/mongo/bson/bsonobjbuilder.h — BSONObjBuilder::obj()
// ---------------------------------------------------------------------------

class BSONObj {
public:
    BSONObj();

    // Model: objdata() returns a pointer to fully-written BSON bytes.
    // Marks the returned buffer as initialized to suppress UNINIT on
    // field reads that follow a call to objdata().
    const char* objdata() const {
        const char* result;
        __coverity_writeall__(&result);
        return result;
    }

    // Model: getOwned() returns an initialized copy of this BSONObj.
    BSONObj getOwned() const {
        BSONObj result;
        __coverity_writeall__(&result);
        return result;
    }
};

}  // namespace mongo
