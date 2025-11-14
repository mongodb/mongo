/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/host/document_source_extension_optimizable.h"

namespace mongo::extension::host {

ALLOCATE_DOCUMENT_SOURCE_ID(extensionOptimizable, DocumentSourceExtensionOptimizable::id);

Value DocumentSourceExtensionOptimizable::serialize(const SerializationOptions& opts) const {
    tassert(11217800,
            "SerializationOptions should keep literals unchanged while represented as a "
            "DocumentSourceExtensionOptimizable",
            opts.isKeepingLiteralsUnchanged());

    if (opts.isSerializingForExplain()) {
        return Value(_logicalStage.explain(*opts.verbosity));
    } else {
        // Serialize the stage for query execution.
        return Value(_logicalStage.serialize());
    }
    return Value(BSONObj());
}

StageConstraints DocumentSourceExtensionOptimizable::constraints(
    PipelineSplitState pipeState) const {
    // Default properties if unset
    auto constraints = DocumentSourceExtension::constraints(pipeState);

    // Apply potential overrides from static properties.
    if (!_properties.getRequiresInputDocSource()) {
        constraints.setConstraintsForNoInputSources();
    }
    if (auto pos = static_properties_util::toPositionRequirement(_properties.getPosition())) {
        constraints.requiredPosition = *pos;
    }
    if (auto host = static_properties_util::toHostTypeRequirement(_properties.getHostType())) {
        constraints.hostRequirement = *host;
    }

    return constraints;
}

DocumentSource::Id DocumentSourceExtensionOptimizable::getId() const {
    return id;
}

}  // namespace mongo::extension::host
