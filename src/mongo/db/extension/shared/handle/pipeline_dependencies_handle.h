// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional.hpp>

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

    /**
     * Returns a BSONObj containing a BSON array of dotted field-path strings representing
     * the specific fields needed by the downstream pipeline. Returns boost::none when
     * needs_whole_document is true.
     */
    boost::optional<BSONObj> getNeededFields() const {
        ::MongoExtensionByteBuf* buf{nullptr};
        invokeCAndConvertStatusToException(
            [&]() { return _vtable().get_needed_fields(get(), &buf); });
        if (!buf) {
            return boost::none;
        }
        ExtensionByteBufHandle ownedBuf{buf};
        return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
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
        tassert(12556400,
                "PipelineDependencies 'get_needed_fields' is null",
                vtable.get_needed_fields != nullptr);
    }
};

}  // namespace mongo::extension
