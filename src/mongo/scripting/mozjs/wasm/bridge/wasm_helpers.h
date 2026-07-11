// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/shared_buffer.h"

#include <fstream>
#include <string>
#include <string_view>

#include <wasmtime.hh>

#include <boost/optional.hpp>
#include <wasmtime/component.hh>
#include <wasmtime/component/val.h>

namespace mongo::mozjs::wasm {
namespace wt = wasmtime;
namespace wc = wasmtime::component;
namespace wasm_helpers {

std::vector<uint8_t> readWasmFile(const std::string& path);

// Translates a WIT wasm-mozjs-error record to a human-readable string.
std::string translateMozJSError(const wc::Val& mozJSError);

// Extracts the mongo-code field from a wasm-mozjs-error record.
// Returns JSInterpreterFailure if the field is absent or zero.
ErrorCodes::Error mozJSErrorCode(const wc::Val& mozJSError);

const wc::Val* findField(std::string_view name, const wc::Record& record);

wc::Func getMozjsFunc(wc::Instance& instance,
                      wt::Store::Context ctx,
                      std::string_view ifaceName,
                      std::string_view funcName);

// Like getMozjsFunc but returns boost::none when the function is not exported.
// Used for optional WIT functions that may not be present in older modules.
boost::optional<wc::Func> getMozjsFuncOptional(wc::Instance& instance,
                                               wt::Store::Context ctx,
                                               std::string_view ifaceName,
                                               std::string_view funcName);


wc::List makeListU8(const uint8_t* data, size_t len);

wc::List makeListU8(std::string_view s);

// Fast path for passing large byte arrays to WASM component functions typed
// as list<u8>.  Returns a Val carrying a RAW_U8_LIST kind that is bulk-copied
// into WASM linear memory when lowered, avoiding N×32-byte per-byte boxing.
[[nodiscard]] wc::Val makeBytesVal(const uint8_t* data, size_t len);

// Create a component Val for WIT `string` type (distinct from list<u8>).
wc::Val makeString(std::string_view s);

std::vector<uint8_t> extractListU8(const wc::Val& v);

// Copies `size` bytes of guest-produced data into an owned BSONObj, fully validating that the
// bytes form a structurally sound BSON document contained within `size`.
//
// The WASM guest (a SpiderMonkey sandbox running arbitrary attacker-supplied JS) is untrusted:
// a compromised guest can forge the BSON length header so it points past the allocation, which
// would otherwise cause downstream BSON readers to over-read adjacent mongod heap into query
// results. `validateBSON` is called with the actual buffer size as maxLength so any embedded
// length that would read past the allocation is rejected. Malformed input throws a uassert
// (never a fatal invariant) so a compromised sandbox can only fail its own request rather than
// leak heap or abort the process. Buffers outside [kMinBSONLength, BufferMaxSize] are rejected
// before allocation.
BSONObj validatedBsonFromGuestBytes(const uint8_t* data, size_t size);

// As validatedBsonFromGuestBytes, but adopts an already-filled `size`-byte SharedBuffer instead of
// copying, avoiding a second allocation on the per-element slow path.
BSONObj validatedBsonFromGuestBuffer(SharedBuffer buf, size_t size);

bool isResultOk(const wc::Val& result);

// Returns true if the WIT error record carries a fatal error code (e-oom or e-internal)
// that leaves the engine in an unrecoverable state.
bool isFatalWitError(const wc::Val& witError);

// Returns true if the WIT error record indicates a non-fatal OOM: an e-runtime
// with an allocation failure message. Fatal OOM (e-oom) is handled by
// isFatalWitError instead.
bool isOomWitError(const wc::Val& witError);

// Converts a BSONObj to a wc::Val (list<u8>) suitable for passing to WASM component functions.
// The bytes are not copied into WASM linear memory until the function is actually invoked
// (lowering). Call this before starting the deadline timer so the O(N) work is not charged
// against the JS function timeout.
[[nodiscard]] wc::Val convertBsonToWcVal(const BSONObj& bson);
}  // namespace wasm_helpers
}  // namespace mongo::mozjs::wasm
