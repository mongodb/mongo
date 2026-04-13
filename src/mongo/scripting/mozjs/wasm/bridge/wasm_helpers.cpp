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

#include "mongo/scripting/mozjs/wasm/bridge/wasm_helpers.h"

namespace mongo::mozjs::wasm::wasm_helpers {

std::vector<uint8_t> readWasmFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return {};
    }
    auto pos = f.tellg();
    if (pos < 0) {
        return {};
    }
    auto size = static_cast<size_t>(pos);
    f.seekg(0);
    std::vector<uint8_t> buf(size);
    if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size))) {
        return {};
    }
    return buf;
}

const wc::Val* findField(std::string_view name, const wc::Record& record) {
    for (auto it = record.begin(); it != record.end(); ++it) {
        if (it->name() == name)
            return &it->value();
    }
    return nullptr;
}

std::string translateMozJSError(const wc::Val& mozJSError) {
    if (!mozJSError.is_record()) {
        return "unknown error (not a record)";
    }

    const auto& record = mozJSError.get_record();

    auto findField = [&](std::string_view name) -> const wc::Val* {
        return wasm_helpers::findField(name, record);
    };

    auto optStr = [](const wc::Val* v) -> std::string {
        if (!v || !v->is_option())
            return "none";
        auto optVal = v->get_option().value();
        return optVal ? std::string(optVal->get_string()) : "none";
    };

    std::stringstream ss;
    // Include the WIT error-code enum (e.g. e-compile, e-runtime, e-oom).
    auto it = record.begin();
    if (it != record.end() && it->value().is_enum()) {
        ss << it->value().get_enum() << " ";
    }
    ss << "message : '" << optStr(findField("msg")) << "', ";
    ss << "file : '" << optStr(findField("filename")) << "', ";
    ss << "stack : '" << optStr(findField("stack")) << "', ";
    if (auto* line = findField("line"); line && line->is_u32()) {
        ss << "line : " << line->get_u32() << ", ";
    }
    if (auto* col = findField("column"); col && col->is_u32()) {
        ss << "column : " << col->get_u32();
    }
    return ss.str();
}

wc::Func getMozjsFunc(wc::Instance& instance,
                      wt::Store::Context ctx,
                      std::string_view ifaceName,
                      std::string_view funcName) {
    auto ifaceIdx = instance.get_export_index(ctx, nullptr, ifaceName);
    // We should never request a function that we don't know of.
    invariant(ifaceIdx);
    auto funcIdx = instance.get_export_index(ctx, &*ifaceIdx, funcName);
    invariant(funcIdx);
    auto func = instance.get_func(ctx, *funcIdx);
    invariant(func);
    return *func;
}

wc::List makeListU8(const uint8_t* data, size_t len) {
    wasmtime_component_vallist_t raw;
    wasmtime_component_vallist_new_uninit(&raw, len);
    for (size_t i = 0; i < len; i++) {
        raw.data[i].kind = WASMTIME_COMPONENT_U8;
        raw.data[i].of.u8 = data[i];
    }
    return wc::List(std::move(raw));
}

wc::List makeListU8(std::string_view s) {
    return makeListU8(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Create a component Val for WIT `string` type (distinct from list<u8>).
wc::Val makeString(std::string_view s) {
    wasmtime_component_val_t raw;
    raw.kind = WASMTIME_COMPONENT_STRING;
    wasm_byte_vec_new(&raw.of.string, s.size(), s.data());
    return wc::Val(std::move(raw));
}

wc::List makeListU8(const BSONObj& obj) {
    return makeListU8(reinterpret_cast<const uint8_t*>(obj.objdata()),
                      static_cast<size_t>(obj.objsize()));
}

std::vector<uint8_t> extractListU8(const wc::Val& v) {
    std::vector<uint8_t> out;
    if (!v.is_list())
        return out;
    const wc::List& list = v.get_list();
    out.reserve(list.size());
    for (const wc::Val& elem : list) {
        if (elem.is_u8()) {
            out.push_back(elem.get_u8());
        }
    }
    return out;
}

bool isResultOk(const wc::Val& result) {
    return result.is_result() && result.get_result().is_ok();
}

bool isFatalWitError(const wc::Val& witError) {
    if (!witError.is_record())
        return false;
    const auto& record = witError.get_record();
    // The first field of the WIT wasm-mozjs-error record is the err-code enum.
    auto it = record.begin();
    if (it == record.end() || !it->value().is_enum())
        return false;
    auto code = it->value().get_enum();
    return code == std::string_view("e-oom") || code == std::string_view("e-internal");
}

bool isOomWitError(const wc::Val& witError) {
    if (!witError.is_record())
        return false;
    const auto& record = witError.get_record();
    auto it = record.begin();
    if (it == record.end() || !it->value().is_enum())
        return false;
    auto code = it->value().get_enum();
    // e-oom is handled by isFatalWitError (sets Trapped). This function only
    // detects the non-fatal OOM path: SpiderMonkey reports heap allocation
    // failures as e-runtime with a message that indicates memory exhaustion.
    //
    // NOTE: This relies on SpiderMonkey error message strings, which are set in
    // js/src/js.msg (JSMSG_ALLOC_OVERFLOW -> "allocation size overflow") and
    // (JSMSG_OUT_OF_MEMORY -> "out of memory"). If SpiderMonkey changes these
    // messages, this detection will silently stop matching and the error will
    // be treated as a generic e-runtime failure rather than OOM.
    if (code == std::string_view("e-runtime")) {
        for (auto fi = record.begin(); fi != record.end(); ++fi) {
            if (fi->name() == std::string_view("msg") && fi->value().is_option()) {
                auto optVal = fi->value().get_option().value();
                if (optVal) {
                    auto msg = optVal->get_string();
                    if (msg.find("allocation size overflow") != std::string_view::npos ||
                        msg.find("out of memory") != std::string_view::npos)
                        return true;
                }
            }
        }
    }
    return false;
}
}  // namespace mongo::mozjs::wasm::wasm_helpers
