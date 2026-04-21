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

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::extension {

class PipelineDependenciesAPI;

template <>
struct c_api_to_cpp_api<::MongoExtensionPipelineDependencies> {
    using CppApi_t = PipelineDependenciesAPI;
};

using PipelineDependenciesHandle = UnownedHandle<const ::MongoExtensionPipelineDependencies>;

class PipelineDependenciesAPI : public VTableAPI<::MongoExtensionPipelineDependencies> {
public:
    PipelineDependenciesAPI(::MongoExtensionPipelineDependencies* ptr)
        : VTableAPI<::MongoExtensionPipelineDependencies>(ptr) {}

    /**
     * Returns true if the pipeline depends on the metadata field identified by 'name'
     * (e.g. "searchSequenceToken").
     */
    bool needsMetadata(std::string_view name) const {
        bool result = false;
        invokeCAndConvertStatusToException(
            [&]() { return _vtable().needs_metadata(get(), stringViewAsByteView(name), &result); });
        return result;
    }

    /**
     * Returns true if the pipeline references the builtin variable identified by 'name'
     * (e.g. "SEARCH_META").
     */
    bool needsVariable(std::string_view name) const {
        bool result = false;
        invokeCAndConvertStatusToException(
            [&]() { return _vtable().needs_variable(get(), stringViewAsByteView(name), &result); });
        return result;
    }

    /**
     * Returns true if the pipeline requires the full document.
     */
    bool needsWholeDocument() const {
        bool result = false;
        invokeCAndConvertStatusToException(
            [&]() { return _vtable().needs_whole_document(get(), &result); });
        return result;
    }

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(12200101,
                "PipelineDependencies 'needs_metadata' is null",
                vtable.needs_metadata != nullptr);
        tassert(12200102,
                "PipelineDependencies 'needs_variable' is null",
                vtable.needs_variable != nullptr);
        tassert(12200103,
                "PipelineDependencies 'needs_whole_document' is null",
                vtable.needs_whole_document != nullptr);
    }
};

}  // namespace mongo::extension
