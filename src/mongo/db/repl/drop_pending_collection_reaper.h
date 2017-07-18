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


#pragma once

#include <boost/optional.hpp>
#include <map>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class OperationContext;
class ServiceContext;

namespace repl {

class StorageInterface;

/**
 * This class manages collections that are marked as drop-pending by the 2-phase algorithm.
 *
 * Replicated collections that are dropped are not permanently removed from the storage system
 * when the drop collection request is first processed. Instead, the collection is renamed to a
 * hidden collection in the same database with a special namespace (<db>.system.drop.*).
 *
 * On receiving a notification in the replica set state (eg. commit level), some drop-pending
 * with drop optimes will become safe to remove permanently. This class provides the function
 * dropCollectionsOlderThan() for this purpose.
 */
class DropPendingCollectionReaper {
    MONGO_DISALLOW_COPYING(DropPendingCollectionReaper);

public:
    // Operation Context binding.
    static DropPendingCollectionReaper* get(ServiceContext* service);
    static DropPendingCollectionReaper* get(ServiceContext& service);
    static DropPendingCollectionReaper* get(OperationContext* opCtx);
    static void set(ServiceContext* service,
                    std::unique_ptr<DropPendingCollectionReaper> storageInterface);

    // Container type for drop-pending namespaces. We use a multimap so that we can order the
    // namespaces by drop optime. Additionally, it is possible for certain user operations (such
    // as renameCollection across databases) to generate more than one drop-pending namespace for
    // the same drop optime.
    using DropPendingNamespaces = std::multimap<OpTime, NamespaceString>;

    explicit DropPendingCollectionReaper(StorageInterface* storageInterface);
    virtual ~DropPendingCollectionReaper() = default;

    /**
     * Adds a new drop-pending namespace, with its drop optime, to be managed by this class.
     */
    void addDropPendingNamespace(const OpTime& dropOpTime,
                                 const NamespaceString& dropPendingNamespace);

    /**
     * Returns earliest drop optime in '_dropPendingNamespaces'.
     * Returns boost::none if '_dropPendingNamespaces' is empty.
     */
    boost::optional<OpTime> getEarliestDropOpTime();

    /**
     * Notifies this class of a change in the replica set state (eg. commit level).
     * Drops all drop-pending namespaces with drop optimes before or at 'opTime'.
     * After this function returns, all entries in '_dropPendingNamespaces' will have drop
     * optimes more recent than 'opTime'.
     */
    void dropCollectionsOlderThan(OperationContext* opCtx, const OpTime& opTime);

    /**
     * Renames the drop-pending namespace at the specified optime back to the provided name.
     * There can only be one matching collection per database and at most two entries per optime
     * (due to renameCollection across databases).
     * We cannot reconstruct the original namespace so we must get it passed in. It accepts the
     * fully qualified namespace so that we can locate the correct entry by optime and database.
     * This function also removes the entry from '_dropPendingNamespaces'.
     * This function returns false if there is no drop-pending collection at the specified optime.
     */
    bool rollBackDropPendingCollection(OperationContext* opCtx,
                                       const OpTime& opTime,
                                       const NamespaceString& collectionNamespace);

private:
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (M)  Reads and writes guarded by _mutex.

    // Guards access to member variables.
    stdx::mutex _mutex;

    // Used to access the storage layer.
    StorageInterface* const _storageInterface;  // (R)

    // Drop-pending namespaces. Ordered by drop optime.
    DropPendingNamespaces _dropPendingNamespaces;  // (M)
};

}  // namespace repl
}  // namespace mongo
