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

#include "mongo/base/status.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {
/**
 * Holds a routing table and whether it has been validated by a shard.
 */
struct RoutingInfoEntry {
    CollectionRoutingInfo cri;
    bool validated;
};

/**
 * RoutingContext provides a structured interface for accessing routing table information via the
 * CatalogCache during a routing operation. It should be instantiated at the start of a query
 * routing operation to safely acquire routing information for a list of namespaces. The routing
 * table data is immutable once the context is created. Its lifetime spans the duration of the
 * operation and must persist until the routing tables have been validated by issuing a versioned
 * request to a shard.
 */
class RoutingContext {
public:
    // TODO SERVER-102931: Integrate the RouterAcquisitionSnapshot
    RoutingContext(OperationContext* opCtx,
                   const std::vector<NamespaceString>&
                       nssList,  // list of required namespaces for the routing operation
                   bool allowLocks = false);

    // TODO SERVER-102931: Integrate the RouterAcquisitionSnapshot
    /**
     * Constructs a RoutingContext from pre-acquired routing tables.
     */
    RoutingContext(OperationContext* opCtx,
                   const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& nssToCriMap);

    // Non-copyable, non-movable
    RoutingContext(const RoutingContext&) = delete;
    RoutingContext& operator=(const RoutingContext&) = delete;

    /**
     * Create a RoutingContext with a synthetic routing table (without using a CatalogCache). Use
     * this for testing or specific cases where fetching from the cache is not required.
     */
    static std::unique_ptr<RoutingContext> createSynthetic(
        stdx::unordered_map<NamespaceString, CollectionRoutingInfo> nssMap);

    /**
     * Returns the routing table mapped to a namespace. This is initialized at construction time
     * and remains the same throughout the routing operation.
     */
    const CollectionRoutingInfo& getCollectionRoutingInfo(const NamespaceString& nss) const;

    /**
     * Returns if the RoutingContext contains the specified NamespaceString.
     */
    bool hasNss(const NamespaceString& nss) const;

    /**
     * Record that a versioned request for a namespace was sent to a shard. The namespace is
     * considered validated.
     */
    virtual void onRequestSentForNss(const NamespaceString& nss);

    /**
     * Utility to trigger CatalogCache refreshes for staleness errors directly from the
     * RoutingContext.
     */
    virtual void onStaleError(const Status& status,
                              boost::optional<const NamespaceString&> nss = boost::none);

    /**
     * By default, the RoutingContext should be validated by running validateOnContextEnd() at the
     * end of a routing operation. This sets _skipValidation to true if it is not a correctness bug
     * for the RoutingContext to terminate without sending a request to a shard.
     */
    void skipValidation();

    /**
     * Sometimes a RoutingContext is needed to determine if a request to a shard will actually need
     * metadata for a request. If the routing operation decides it no longer needs that metadata
     * then this method releases it so it can't be used.
     */
    void release(const NamespaceString& nss);

    /**
     * Validate the RoutingContext prior to destruction to ensure that either:
     * 1. All declared namespaces have had their routing tables validated by sending a versioned
     * request to a shard.
     * 2. An exception is thrown (i.e. if the collection generation has changed) and will be
     * propagated up the stack.
     *
     * It is considered a logic bug if a RoutingContext goes out of scope and neither of the above
     * are true, unless skipValidation() is explicitly set.
     */
    void validateOnContextEnd() const;

    virtual ~RoutingContext() = default;

protected:
    RoutingContext(stdx::unordered_map<NamespaceString, CollectionRoutingInfo> nssMap);

private:
    /**
     * Obtain the routing table associated with a namespace from the CatalogCache. This returns the
     * latest routing table unless the read concern is snapshot with "atClusterTime" set. If an
     * error occurs, this returns a failed status.
     */
    StatusWith<CollectionRoutingInfo> _getCollectionRoutingInfo(OperationContext* opCtx,
                                                                const NamespaceString& nss,
                                                                bool allowLocks) const;

    CatalogCache* _catalogCache;

    using NssRoutingInfoMap = stdx::unordered_map<NamespaceString, RoutingInfoEntry>;
    NssRoutingInfoMap _nssRoutingInfoMap;
    stdx::unordered_set<NamespaceString> _releasedNssList;

    // If set, skip validation prior to destruction that the RoutingContext has had its routing
    // tables validated by sending a versioned request to a shard
    bool _skipValidation = false;
};

class MockRoutingContext : public RoutingContext {
public:
    MockRoutingContext(stdx::unordered_map<NamespaceString, CollectionRoutingInfo> map = {})
        : RoutingContext(map) {}
    // TODO SERVER-102931: Integrate the RouterAcquisitionSnapshot
    MockRoutingContext(OperationContext* opCtx,
                       const std::vector<NamespaceString>&
                           nssList,  // list of required namespaces for the routing operation
                       bool allowLocks = false)
        : RoutingContext(opCtx, nssList, allowLocks) {}

    // TODO SERVER-102931: Integrate the RouterAcquisitionSnapshot
    MockRoutingContext(
        OperationContext* opCtx,
        const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& nssToCriMap)
        : RoutingContext(opCtx, nssToCriMap) {}

    // Non-copyable, non-movable
    MockRoutingContext(const RoutingContext&) = delete;
    MockRoutingContext& operator=(const RoutingContext&) = delete;

    void onRequestSentForNss(const NamespaceString& nss) override {}
    void onStaleError(const Status& status, boost::optional<const NamespaceString&> nss) override {
        if (nss.has_value()) {
            errors.emplace(*nss, status);
        }
    };
    stdx::unordered_map<NamespaceString, Status> errors;
};

namespace routing_context_utils {
/*
 * Invoke a callback with a RoutingContext and call validateOnContextEnd() if it successfully
 * finishes to ensure every declared namespace had its routing table validated by sending a
 * versioned request to a shard.
 */
template <class Fn>
auto runAndValidate(RoutingContext& routingCtx, Fn&& fn) {
    using ReturnType = std::invoke_result_t<Fn, RoutingContext&>;
    if constexpr (std::is_void_v<ReturnType>) {
        fn(routingCtx);
        routingCtx.validateOnContextEnd();
    } else {
        ReturnType res = fn(routingCtx);
        routingCtx.validateOnContextEnd();
        return res;
    }
}

/*
 * Construct a RoutingContext directly for a list of namespaces, run the callback, and check that
 * all acquired routing tables were validated against a shard.
 */
template <class Fn>
auto withValidatedRoutingContext(OperationContext* opCtx,
                                 const std::vector<NamespaceString>& nssList,
                                 Fn&& fn) {
    RoutingContext routingCtx(opCtx, nssList);
    return runAndValidate(routingCtx, std::forward<Fn>(fn));
}

/*
 * Same as above, but checks whether locks are allowed if we are in a transaction.
 */
template <class Fn>
auto withValidatedRoutingContextForTxnCmd(OperationContext* opCtx,
                                          const std::vector<NamespaceString>& nssList,
                                          Fn&& fn) {
    // When in a multi-document transaction, allow getting routing info from the
    // CatalogCache even though locks may be held. The CatalogCache will throw
    // CannotRefreshDueToLocksHeld if the entry is not already cached.
    const auto allowLocks =
        opCtx->inMultiDocumentTransaction() && shard_role_details::getLocker(opCtx)->isLocked();
    RoutingContext routingCtx(opCtx, nssList, allowLocks);
    return runAndValidate(routingCtx, std::forward<Fn>(fn));
}

/*
 * Construct a RoutingContext directly from a pre-computed map of nss->CollectionRoutingInfo pairs,
 * run the callback, and check that all acquired routing tables were validated against a shard.
 */
template <class Fn>
auto withValidatedRoutingContext(
    OperationContext* opCtx,
    const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& nssToCriMap,
    Fn&& fn) {
    RoutingContext routingCtx(opCtx, nssToCriMap);
    return runAndValidate(routingCtx, std::forward<Fn>(fn));
}
}  // namespace routing_context_utils
}  // namespace mongo
