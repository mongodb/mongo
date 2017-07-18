/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/drop_pending_collection_reaper.h"

#include <algorithm>
#include <utility>

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {


namespace {

const auto getDropPendingCollectionReaper =
    ServiceContext::declareDecoration<std::unique_ptr<DropPendingCollectionReaper>>();

}  // namespace

DropPendingCollectionReaper* DropPendingCollectionReaper::get(ServiceContext* service) {
    return getDropPendingCollectionReaper(service).get();
}

DropPendingCollectionReaper* DropPendingCollectionReaper::get(ServiceContext& service) {
    return getDropPendingCollectionReaper(service).get();
}

DropPendingCollectionReaper* DropPendingCollectionReaper::get(OperationContext* opCtx) {
    return get(opCtx->getClient()->getServiceContext());
}


void DropPendingCollectionReaper::set(ServiceContext* service,
                                      std::unique_ptr<DropPendingCollectionReaper> newReaper) {
    auto& reaper = getDropPendingCollectionReaper(service);
    reaper = std::move(newReaper);
}

DropPendingCollectionReaper::DropPendingCollectionReaper(StorageInterface* storageInterface)
    : _storageInterface(storageInterface) {}

void DropPendingCollectionReaper::addDropPendingNamespace(
    const OpTime& dropOpTime, const NamespaceString& dropPendingNamespace) {
    invariant(dropPendingNamespace.isDropPendingNamespace());
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    const auto equalRange = _dropPendingNamespaces.equal_range(dropOpTime);
    const auto& lowerBound = equalRange.first;
    const auto& upperBound = equalRange.second;
    auto matcher = [&dropPendingNamespace](const auto& pair) {
        return pair.second == dropPendingNamespace;
    };
    if (std::find_if(lowerBound, upperBound, matcher) == upperBound) {
        _dropPendingNamespaces.insert(std::make_pair(dropOpTime, dropPendingNamespace));
    } else {
        severe() << "Failed to add drop-pending collection " << dropPendingNamespace
                 << " with drop optime " << dropOpTime << ": duplicate optime and namespace pair.";
        fassertFailedNoTrace(40448);
    }
}

boost::optional<OpTime> DropPendingCollectionReaper::getEarliestDropOpTime() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto it = _dropPendingNamespaces.cbegin();
    if (it == _dropPendingNamespaces.cend()) {
        return boost::none;
    }
    return it->first;
}

bool DropPendingCollectionReaper::rollBackDropPendingCollection(
    OperationContext* opCtx, const OpTime& opTime, const NamespaceString& collectionNamespace) {
    // renames because these are internal operations.
    UnreplicatedWritesBlock uwb(opCtx);

    const auto pendingNss = collectionNamespace.makeDropPendingNamespace(opTime);
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        const auto equalRange = _dropPendingNamespaces.equal_range(opTime);
        const auto& lowerBound = equalRange.first;
        const auto& upperBound = equalRange.second;
        auto matcher = [&pendingNss](const auto& pair) { return pair.second == pendingNss; };
        auto it = std::find_if(lowerBound, upperBound, matcher);
        if (it == upperBound) {
            warning() << "Cannot find drop-pending namespace at OpTime " << opTime
                      << " for collection " << collectionNamespace << " to roll back.";
            return false;
        }

        _dropPendingNamespaces.erase(it);
    }

    log() << "Rolling back collection drop for " << pendingNss << " with drop OpTime " << opTime
          << " to namespace " << collectionNamespace;

    auto status = _storageInterface->renameCollection(opCtx, pendingNss, collectionNamespace, true);
    if (!status.isOK()) {
        warning() << "Failed to roll back drop-pending collection " << pendingNss
                  << " with drop OpTime " << opTime << " and rename it to namespace "
                  << collectionNamespace << ": " << status;
    }

    return true;
}

void DropPendingCollectionReaper::dropCollectionsOlderThan(OperationContext* opCtx,
                                                           const OpTime& opTime) {
    DropPendingNamespaces toDrop;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        auto it = _dropPendingNamespaces.cbegin();
        while (it != _dropPendingNamespaces.cend() && it->first <= opTime) {
            toDrop.insert(*it);
            it = _dropPendingNamespaces.erase(it);
        }
    }

    if (toDrop.empty()) {
        return;
    }

    // Every node cleans up its own drop-pending collections. We should never replicate these drops
    // because these are internal operations.
    UnreplicatedWritesBlock uwb(opCtx);

    for (const auto& opTimeAndNamespace : toDrop) {
        const auto& dropOpTime = opTimeAndNamespace.first;
        const auto& nss = opTimeAndNamespace.second;
        log() << "Completing collection drop for " << nss << " with drop optime " << dropOpTime
              << " (notification optime: " << opTime << ")";
        auto status = _storageInterface->dropCollection(opCtx, nss);
        if (!status.isOK()) {
            warning() << "Failed to remove drop-pending collection " << nss << " with drop optime "
                      << dropOpTime << " (notification optime: " << opTime << "): " << status;
        }
    }
}

}  // namespace repl
}  // namespace mongo
