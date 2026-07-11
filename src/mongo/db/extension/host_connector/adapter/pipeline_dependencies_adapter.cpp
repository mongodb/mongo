// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host_connector/adapter/pipeline_dependencies_adapter.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"

#include <string_view>

namespace mongo::extension::host_connector {

::MongoExtensionStatus* PipelineDependenciesAdapter::_hostNeedsMetadata(
    const ::MongoExtensionPipelineDependencies* deps,
    ::MongoExtensionByteView name,
    bool* result) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto sv = byteViewAsStringView(name);
        *result = static_cast<const PipelineDependenciesAdapter*>(deps)->getImpl().getNeedsMetadata(
            DocumentMetadataFields::parseMetaType(sv));
    });
}

::MongoExtensionStatus* PipelineDependenciesAdapter::_hostNeedsVariable(
    const ::MongoExtensionPipelineDependencies* deps,
    ::MongoExtensionByteView name,
    bool* result) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto sv = byteViewAsStringView(name);
        const auto& variableRefs =
            static_cast<const PipelineDependenciesAdapter*>(deps)->getVariableRefs();
        *result = variableRefs.contains(std::string(sv));
    });
}

::MongoExtensionStatus* PipelineDependenciesAdapter::_hostNeedsWholeDocument(
    const ::MongoExtensionPipelineDependencies* deps, bool* result) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *result =
            static_cast<const PipelineDependenciesAdapter*>(deps)->getImpl().needWholeDocument;
    });
}

::MongoExtensionStatus* PipelineDependenciesAdapter::_hostGetNeededFields(
    const ::MongoExtensionPipelineDependencies* deps, ::MongoExtensionByteBuf** result) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& impl = static_cast<const PipelineDependenciesAdapter*>(deps)->getImpl();
        if (impl.needWholeDocument) {
            *result = nullptr;
            return;
        }
        BSONArrayBuilder builder;
        for (const auto& field : impl.fields) {
            builder.append(field);
        }
        *result = new ByteBuf(builder.arr());
    });
}

}  // namespace mongo::extension::host_connector
