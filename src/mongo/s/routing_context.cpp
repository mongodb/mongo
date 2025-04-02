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


#include "mongo/s/routing_context.h"

#include <optional>

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {
boost::optional<Timestamp> getEffectiveAtClusterTime(OperationContext* opCtx) {
    if (auto txnRouter = TransactionRouter::get(opCtx)) {
        // Check to see if we're running in a transaction with snapshot level read concern.
        if (auto atClusterTime = txnRouter.getSelectedAtClusterTime()) {
            return atClusterTime->asTimestamp();
        }
    }

    if (auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
        // Check the read concern for the atClusterTime argument.
        return atClusterTime->asTimestamp();
    }

    // Otherwise, the latest routing table is sufficient.
    return boost::none;
}
}  // namespace

RoutingContext::RoutingContext(OperationContext* opCtx,
                               const std::vector<NamespaceString>& nssList,
                               bool allowLocks)
    : _catalogCache(Grid::get(opCtx)->catalogCache()), _nssToCriMap([&] {
          auto map = stdx::unordered_map<NamespaceString, CollectionRoutingInfo>{};
          map.reserve(nssList.size());

          for (const auto& nss : nssList) {
              const auto cri = uassertStatusOK(_getCollectionRoutingInfo(opCtx, nss, allowLocks));
              auto [it, inserted] = map.try_emplace(nss, std::move(cri));
              tassert(10292300,
                      str::stream() << "Namespace " << nss.toStringForErrorMsg()
                                    << " declared multiple times in RoutingContext",
                      inserted);
          }

          return map;
      }()) {}

const CollectionRoutingInfo& RoutingContext::getCollectionRoutingInfo(
    const NamespaceString& nss) const {
    auto it = _nssToCriMap.find(nss);
    uassert(10292301,
            str::stream() << "Attempted to access RoutingContext for undeclared namespace "
                          << nss.toStringForErrorMsg(),
            it != _nssToCriMap.end());
    return it->second;
}

StatusWith<CollectionRoutingInfo> RoutingContext::_getCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss, bool allowLocks) const {
    if (auto atClusterTime = getEffectiveAtClusterTime(opCtx)) {
        return _catalogCache->getCollectionRoutingInfoAt(opCtx, nss, *atClusterTime, allowLocks);
    } else {
        return _catalogCache->getCollectionRoutingInfo(opCtx, nss, allowLocks);
    }
}
}  // namespace mongo
