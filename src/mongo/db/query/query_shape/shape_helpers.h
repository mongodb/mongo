// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/modules.h"

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
BSONObj extractHintShape(const BSONObj& hintObj, const query_shape::SerializationOptions& opts);
BSONObj extractMinOrMaxShape(const BSONObj& obj, const query_shape::SerializationOptions& opts);

void appendNamespaceShape(BSONObjBuilder& bob,
                          const NamespaceString& nss,
                          const query_shape::SerializationOptions& opts);

/**
 * Evaluates the 'deferredShape' and computes a QueryShapeHash if both the shape and the client
 * are eligible. If not eligible, returns boost::none.
 *
 * Both overloads share the same core eligibility checks:
 *   - Skip internal clients (unless 'skipInternalClientCheck' is true — passed by callers that
 *     want the hash recorded on the shard side of a sharded write).
 *   - Skip direct clients (DBDirectClient).
 *   - Skip queries against internal databases or system collections.
 *   - Return boost::none if the deferred shape failed to evaluate.
 *
 * The ExpressionContext overload additionally short-circuits for:
 *   - IDHACK fast-path queries (find / update by _id).
 *   - FLE queries (flagged on the ExpressionContext).
 * Use this overload from find / update / delete / distinct / count / agg paths, which already
 * construct an ExpressionContext as part of query parsing.
 *
 * The OperationContext overload runs only the core checks. Use it from commands that don't
 * construct an ExpressionContext — notably insert, where IDHACK does not apply and FLE queries
 * are screened earlier (via 'wholeOp.getEncryptionInformation()' at the insert entry point).
 */
boost::optional<query_shape::QueryShapeHash> computeQueryShapeHash(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const query_shape::DeferredQueryShape& deferredShape,
    const NamespaceString& nss,
    bool skipInternalClientCheck = false);

boost::optional<query_shape::QueryShapeHash> computeQueryShapeHash(
    OperationContext* opCtx,
    const query_shape::DeferredQueryShape& deferredShape,
    const NamespaceString& nss,
    bool skipInternalClientCheck = false);
}  // namespace mongo::shape_helpers
