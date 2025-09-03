/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/logv2/log.h"

namespace mongo::shape_helpers {

template <typename T, typename Request, typename... Args>
StatusWith<std::unique_ptr<T>> tryMakeShape(Request&& request, Args&&... args) {
    try {
        return std::make_unique<T>(std::forward<Request>(request), std::forward<Args>(args)...);
    } catch (const DBException& ex) {
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery
        LOGV2_WARNING(8472507,
                      "Failed to compute query shape",
                      "command"_attr = redact(request.toBSON()).toString(),
                      "error"_attr = ex.toString());
#undef MONGO_LOGV2_DEFAULT_COMPONENT
        return ex.toStatus();
    }
}

int64_t inline optionalObjSize(boost::optional<BSONObj> optionalObj) {
    if (!optionalObj)
        return 0;
    return optionalObj->objsize();
}

template <typename T>
int64_t optionalSize(boost::optional<T> optionalVal) {
    if (!optionalVal)
        return 0;
    return optionalVal->size();
}

template <typename T>
std::function<size_t(size_t, const T&)> sizeAccumulatorFunc() {
    MONGO_UNREACHABLE;  // Don't know how to compute the size of this template type.
};

template <>
inline std::function<size_t(size_t, const BSONObj&)> sizeAccumulatorFunc<BSONObj>() {
    return [](size_t total, const BSONObj& obj) {
        return total + sizeof(BSONObj) + static_cast<size_t>(obj.objsize());
    };
}

template <>
inline std::function<size_t(size_t, const NamespaceString&)>
sizeAccumulatorFunc<NamespaceString>() {
    return [](size_t total, const NamespaceString& nss) {
        // For each element, we have to track the size of the
        // nss as well as the size allocated by the nss. It would be
        // ideal to be able to ask the underlying namespace string for
        // its capacity, but it's not something we have access to.
        // Further, namespace strings appear to shrink to fit (i.e
        // resize to correct size), so it may not be necessary. Should
        // we also try to consider short string optimization? At the
        // very least, the current approach gives us a good upper bound
        // memory usage (assuming shrink to fit).
        return total + sizeof(nss) + nss.size();
    };
}

template <typename Container>
size_t containerSize(const Container& container) {
    return std::accumulate(container.begin(),
                           container.end(),
                           0,
                           sizeAccumulatorFunc<typename Container::value_type>());
}

/**
 * Serializes the given 'hintObj' in accordance with the options. Assumes the hint is correct and
 * contains field names. It is possible that this hint doesn't actually represent an index, but we
 * can't detect that here.
 */
BSONObj extractHintShape(const BSONObj& hintObj, const SerializationOptions& opts);
BSONObj extractMinOrMaxShape(const BSONObj& obj, const SerializationOptions& opts);

void appendNamespaceShape(BSONObjBuilder& bob,
                          const NamespaceString& nss,
                          const SerializationOptions& opts);

/**
 * Evaluates the 'deferredShape' and computes a QueryShapeHash if both the shape and the client
 * are eligible. If not eligible, returns boost::none.
 */
boost::optional<query_shape::QueryShapeHash> computeQueryShapeHash(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const query_shape::DeferredQueryShape& deferredShape,
    const NamespaceString& nss);
}  // namespace mongo::shape_helpers
