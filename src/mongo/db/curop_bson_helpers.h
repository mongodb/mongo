// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/modules.h"

#include <string_view>

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
inline void appendObjectTruncatingAsNecessary(std::string_view fieldName,
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
