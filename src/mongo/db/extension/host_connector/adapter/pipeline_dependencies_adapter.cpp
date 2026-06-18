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
