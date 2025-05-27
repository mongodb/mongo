/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/update/object_transform_executor.h"

#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/update/object_replace_executor.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

ObjectTransformExecutor::ObjectTransformExecutor(TransformFunc transformFunc)
    : _transformFunc(std::move(transformFunc)) {}

UpdateExecutor::ApplyResult ObjectTransformExecutor::applyTransformUpdate(
    ApplyParams applyParams, const TransformFunc& transformFunc) {

    auto originalDoc = applyParams.element.getDocument().getObject();
    auto transformedDoc = transformFunc(originalDoc);
    if (!transformedDoc) {
        return ApplyResult::noopResult();
    }

    return ObjectReplaceExecutor::applyReplacementUpdate(
        std::move(applyParams),
        *transformedDoc,
        true /* replacementDocContainsIdField */,
        true /* allowTopLevelDollarPrefixedFields */);
}

UpdateExecutor::ApplyResult ObjectTransformExecutor::applyUpdate(ApplyParams applyParams) const {
    auto ret = applyTransformUpdate(applyParams, _transformFunc);

    if (!ret.noop && applyParams.logMode != ApplyParams::LogMode::kDoNotGenerateOplogEntry) {
        // Using a full replacement oplog entry as the current use case is doing major changes.
        // Consider supporting v:2 update in the future.
        ret.oplogEntry = update_oplog_entry::makeReplacementOplogEntry(
            applyParams.element.getDocument().getObject());
    }
    return ret;
}

Value ObjectTransformExecutor::serialize() const {
    MONGO_UNREACHABLE_TASSERT(5857810);
}
}  // namespace mongo
