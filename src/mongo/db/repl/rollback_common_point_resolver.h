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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/abstract_oplog_fetcher.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/stdx/functional.h"

namespace mongo {
namespace repl {

/**
 * This class finds the common point between a local and remote oplog. It uses an
 * AbstractOplogFetcher to asynchronously fetch entries from the remote oplog and synchonously
 * reads oplog entries from the local oplog using the OplogInterface.
 *
 * The RollbackCommonPointResolver returns the common point through the onCommonPoint function
 * on the Listener.
 *
 * The RollbackCommonPointResolver passes each local oplog entry into onLocalOplogEntry and
 * every remote oplog entry into onRemoteOplogEntry when they are checked to be the common
 * point. It will only call each of these functions once per oplog entry, and will not call
 * onLocalOplogEntry and onRemoteOplogEntry on the common point.
 */
class RollbackCommonPointResolver : public AbstractOplogFetcher {
    MONGO_DISALLOW_COPYING(RollbackCommonPointResolver);

public:
    /**
     * An object representing the common point of two oplogs. It includes the OpTime of the
     * common point and the RecordId of the oplog entry in the local oplog. The RecordId is
     * used to truncate the oplog.
     */
    using RollbackCommonPoint = std::pair<OpTime, RecordId>;

    class Listener {
    public:
        virtual ~Listener() = default;

        /**
         *  Function called to process each local oplog entry. Accepts an oplog entry as
         *  a BSONObj and returns a status on error.
         */
        virtual Status onLocalOplogEntry(const BSONObj& oplogEntryObj) = 0;

        /**
         *  Function called to process each remote oplog entry. Accepts an oplog entry as
         *  a BSONObj and returns a status on error.
         */
        virtual Status onRemoteOplogEntry(const BSONObj& oplogEntryObj) = 0;

        /**
         * Function called when the RollbackCommonPointResolver finds the common point.
         * It accepts the local RecordId of the common point so that rollback can easily
         * truncate the oplog at the common point.
         * Returns a status on error.
         */
        virtual Status onCommonPoint(const RollbackCommonPoint& oplogEntryObj) = 0;
    };

    /**
     * Invariants if validation fails on any of the provided arguments.
     */
    RollbackCommonPointResolver(executor::TaskExecutor* executor,
                                HostAndPort source,
                                NamespaceString nss,
                                std::size_t maxFetcherRestarts,
                                OplogInterface* localOplog,
                                Listener* listener,
                                OnShutdownCallbackFn onShutdownCallbackFn);

    virtual ~RollbackCommonPointResolver();

private:
    Status _doStartup_inlock() noexcept override;

    StatusWith<BSONObj> _onSuccessfulBatch(const Fetcher::QueryResponse& queryResponse) override;

    BSONObj _makeFindCommandObject(const NamespaceString& nss,
                                   OpTime lastOpTimeFetched) const override;

    BSONObj _makeMetadataObject() const override;

    /**
     * Get next local oplog entry and error if we've reached the end of the local oplog.
     */
    StatusWith<OplogInterface::Iterator::Value> _getNextLocalOplogEntry(
        const BSONObj& remoteOplogEntry);

    // The metadata object sent with the Fetcher queries.
    const BSONObj _metadataObject;

    // An iterator to traverse the local oplog backwards.
    OplogInterface* _localOplog;
    std::unique_ptr<OplogInterface::Iterator> _localOplogIterator;

    // A listener with functions to call with each local and each remote oplog entry and
    // the common point.
    Listener* _listener;

    // Counts the number of oplog entries scanned on the local and remote oplogs. These are not
    // protected by a mutex because they are only used in a single thread.
    unsigned long long _localScanned = 0;
    unsigned long long _remoteScanned = 0;

    // We save the local oplog entry we are at so that we can start from the correct place
    // between batches. This is not protected by a mutex because it is only used in a single
    // thread.
    OplogInterface::Iterator::Value _localOplogValue;
};

}  // namespace repl
}  // namespace mongo
