/**
 *    Copyright (C) 2015 MongoDB Inc.
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
#include <vector>

#include "mongo/bson/oid.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/dist_lock_catalog.h"
#include "mongo/util/time_support.h"

namespace mongo {

class NamespaceString;
class ShardRegistry;
struct HostAndPort;
struct ReadPreferenceSetting;

class DistLockCatalogImpl final : public DistLockCatalog {
public:
    DistLockCatalogImpl(ShardRegistry* shardRegistry);

    virtual ~DistLockCatalogImpl();

    virtual StatusWith<LockpingsType> getPing(OperationContext* txn, StringData processID) override;

    virtual Status ping(OperationContext* txn, StringData processID, Date_t ping) override;

    virtual StatusWith<LocksType> grabLock(OperationContext* txn,
                                           StringData lockID,
                                           const OID& lockSessionID,
                                           StringData who,
                                           StringData processId,
                                           Date_t time,
                                           StringData why) override;

    virtual StatusWith<LocksType> overtakeLock(OperationContext* txn,
                                               StringData lockID,
                                               const OID& lockSessionID,
                                               const OID& currentHolderTS,
                                               StringData who,
                                               StringData processId,
                                               Date_t time,
                                               StringData why) override;

    virtual Status unlock(OperationContext* txn, const OID& lockSessionID) override;

    virtual Status unlockAll(OperationContext* txn, const std::string& processID) override;

    virtual StatusWith<ServerInfo> getServerInfo(OperationContext* txn) override;

    virtual StatusWith<LocksType> getLockByTS(OperationContext* txn,
                                              const OID& lockSessionID) override;

    virtual StatusWith<LocksType> getLockByName(OperationContext* txn, StringData name) override;

    virtual Status stopPing(OperationContext* txn, StringData processId) override;

private:
    StatusWith<std::vector<BSONObj>> _findOnConfig(OperationContext* txn,
                                                   const ReadPreferenceSetting& readPref,
                                                   const NamespaceString& nss,
                                                   const BSONObj& query,
                                                   const BSONObj& sort,
                                                   boost::optional<long long> limit);

    ShardRegistry* _client;

    // These are not static to avoid initialization order fiasco.
    const NamespaceString _lockPingNS;
    const NamespaceString _locksNS;
};

}  // namespace mongo
