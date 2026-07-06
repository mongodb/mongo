/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
