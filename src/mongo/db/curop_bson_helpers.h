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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace curop_bson_helpers {
/**
 * Converts 'obj' to a string (excluding the "comment" field), truncates the string to ensure it is
 * no greater than the 'maxSize' limit, and appends a "$truncated" element to 'builder' with the
 * truncated string. If 'obj' has a "comment" field, appends it to 'builder' unchanged.
 */
inline void buildTruncatedObject(const BSONObj& obj, size_t maxSize, BSONObjBuilder& builder) {
    auto comment = obj["comment"];

    auto truncatedObj = (comment.eoo() ? obj : obj.removeField("comment")).toString();

    // 'BSONObj::toString()' abbreviates large strings and deeply nested objects, so it may not be
    // necessary to truncate the string.
    if (truncatedObj.size() > maxSize) {
        LOGV2_INFO(4760300,
                   "Truncating object that exceeds limit for command objects in currentOp results",
                   "size"_attr = truncatedObj.size(),
                   "limit"_attr = maxSize);

        truncatedObj.resize(maxSize - 3);
        truncatedObj.append("...");
    }

    builder.append("$truncated", truncatedObj);
    if (!comment.eoo()) {
        builder.append(comment);
    }
}

/**
 * If 'obj' is smaller than the 'maxSize' limit or if there is no limit, append 'obj' to 'builder'
 * as an element with 'fieldName' as its name. If 'obj' exceeds the limit, append an abbreviated
 * object that preserves the "comment" field (if it exists) but converts the rest to a string that
 * gets truncated to fit the limit.
 */
inline void appendObjectTruncatingAsNecessary(StringData fieldName,
                                              const BSONObj& obj,
                                              boost::optional<size_t> maxSize,
                                              BSONObjBuilder& builder) {
    if (!maxSize || static_cast<size_t>(obj.objsize()) <= maxSize) {
        builder.append(fieldName, obj);
        return;
    }

    BSONObjBuilder truncatedBuilder(builder.subobjStart(fieldName));
    buildTruncatedObject(obj, *maxSize, builder);
    truncatedBuilder.doneFast();
}

/**
 * Appends the command's comment to 'cmdObj'.
 */
inline BSONObj appendCommentField(OperationContext* opCtx, const BSONObj& cmdObj) {
    return opCtx->getComment() && !cmdObj["comment"] ? cmdObj.addField(*opCtx->getComment())
                                                     : cmdObj;
}
}  // namespace curop_bson_helpers
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
