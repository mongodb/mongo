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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/overloaded_visitor.h"

#include <fstream>
#include <string>

#include <wasmtime.hh>

#include <wasmtime/component.hh>
#include <wasmtime/component/val.h>

namespace mongo::mozjs::wasm {
namespace wt = wasmtime;
namespace wc = wasmtime::component;
namespace wasm_helpers {

std::vector<uint8_t> readWasmFile(const std::string& path);

// Translates errors coming from the MozJS interpreter.
std::string translateMozJSError(const wc::Val& mozJSError);

const wc::Val* findField(std::string_view name, const wc::Record& record);

wc::Func getMozjsFunc(wc::Instance& instance,
                      wt::Store::Context ctx,
                      std::string_view ifaceName,
                      std::string_view funcName);

wc::List makeListU8(const uint8_t* data, size_t len);

wc::List makeListU8(std::string_view s);

// Create a component Val for WIT `string` type (distinct from list<u8>).
wc::Val makeString(std::string_view s);

wc::List makeListU8(const BSONObj& obj);

std::vector<uint8_t> extractListU8(const wc::Val& v);

bool isResultOk(const wc::Val& result);

// Returns true if the WIT error record carries a fatal error code (e-oom or e-internal)
// that leaves the engine in an unrecoverable state.
bool isFatalWitError(const wc::Val& witError);

// Returns true if the WIT error record indicates a non-fatal OOM: an e-runtime
// with an allocation failure message. Fatal OOM (e-oom) is handled by
// isFatalWitError instead.
bool isOomWitError(const wc::Val& witError);
}  // namespace wasm_helpers
}  // namespace mongo::mozjs::wasm
