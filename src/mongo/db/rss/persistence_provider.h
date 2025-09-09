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
#include "mongo/bson/bsonelement.h"

#include <string>
#include <utility>

#include <boost/optional.hpp>


namespace mongo {
namespace rss {

/**
 * This class provides an abstraction around the persistence layer underlying the storage and
 * replication subsystems. Depending on the configuration, the implementation may be backed by a
 * local filesystem, a remote service, etc. The interface is built primarily around capabilities and
 * expected behaviors, allowing consumers to act based on these flags, rather than needing to reason
 * about how a particular provider would behave in a given context.
 */
class PersistenceProvider {
public:
    virtual ~PersistenceProvider() = default;

    /**
     * The name of this provider, for use in e.g. logging and error messages.
     */
    virtual std::string name() const = 0;

    /**
     * If not none, the KVEngine will use the returned Timestamp during initialization as the
     * initial data timestamp.
     */
    virtual boost::optional<Timestamp> getSentinelDataTimestamp() const = 0;

    /**
     * Additional configuration that shoudld be added to the WiredTiger config string for the
     * 'wiredtiger_open' call. The 'flattenLeafPageDelta' is expected to be the corresponding
     * WiredTigerConfig member value.
     */
    virtual std::string getWiredTigerConfig(int flattenLeafPageDelta) const = 0;

    /**
     * If true, the provider expects that all catalog identifiers will be replicated and identical
     * between nodes.
     */
    virtual bool shouldUseReplicatedCatalogIdentifiers() const = 0;

    /**
     * If true, the provider expects that RecordIds will be replicated (either explicitly or
     * implicitly) and identical between nodes.
     */
    virtual bool shouldUseReplicatedRecordIds() const = 0;

    /**
     * If true, writes to the oplog should be used as the unit of progress for flow control
     * sampling.
     */
    virtual bool shouldUseOplogWritesForFlowControlSampling() const = 0;

    /**
     * If true, the node should step down prior to shutdown in order to minimize unavailability.
     */
    virtual bool shouldStepDownForShutdown() const = 0;

    /**
     * If true, data may not be availabile immediately after starting the storage engine, so systems
     * like the catalog should not be initialized immediately.
     */
    virtual bool shouldDelayDataAccessDuringStartup() const = 0;

    /**
     * If true, the system should take precautions to avoid taking multiple checkopints for the same
     * stable timestamp. The underlying key-value engine likely does not provide the necessary
     * coordination by default.
     */
    virtual bool shouldAvoidDuplicateCheckpoints() const = 0;

    /**
     * If true, the storage provider supports the use of local, unreplicated collections.
     */
    virtual bool supportsLocalCollections() const = 0;

    /**
     * If true, the provider can support unstable checkpoints.
     */
    virtual bool supportsUnstableCheckpoints() const = 0;

    /**
     * If true, the provider can support logging (i.e. journaling) on individual tables.
     */
    virtual bool supportsTableLogging() const = 0;

    /**
     * If true, the provider supports cross-shard transactions.
     */
    virtual bool supportsCrossShardTransactions() const = 0;
};

}  // namespace rss
}  // namespace mongo
