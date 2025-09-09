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

#include "mongo/db/rss/persistence_provider.h"

namespace mongo::rss {

class AttachedPersistenceProvider : public PersistenceProvider {
public:
    std::string name() const override;

    /**
     * We do not have any specific initialization requirements.
     */
    boost::optional<Timestamp> getSentinelDataTimestamp() const override;

    /**
     * We do not have any additional WT config to add.
     */
    std::string getWiredTigerConfig(int) const override;

    /**
     * Replicated catalog identifiers aren't compatible with attached storage as of right now, as a
     * node may create a local collection whose catalog identifier collides with that of a
     * replicated collection created on another node.
     */
    bool shouldUseReplicatedCatalogIdentifiers() const override;

    /**
     * Attached storage does not require replicated RecordIds to function correctly.
     */
    bool shouldUseReplicatedRecordIds() const override;

    /**
     * Flow control is based on the rate of generation of oplog data and the ability of the
     * secondaries to keep the majority commit point relatively up-to-date.
     */
    bool shouldUseOplogWritesForFlowControlSampling() const override;

    /**
     * Stepping down prior to shut down allows for a graceful and quick election most of the time.
     */
    bool shouldStepDownForShutdown() const override;

    /**
     * We can safely initialize the catalog immediately after starting the storage engine.
     */
    bool shouldDelayDataAccessDuringStartup() const override;

    /**
     * Running a duplicate checkpoint for a given timestamp has little effect other than being
     * slightly inefficient, so there's no need to use extra synchronization to avoid it.
     */
    bool shouldAvoidDuplicateCheckpoints() const override;

    /**
     * We can support local, fully unreplicated collections.
     */
    bool supportsLocalCollections() const override;

    /**
     * We can support unstable checkpoints.
     */
    bool supportsUnstableCheckpoints() const override;

    /**
     * We can support table logging.
     */
    bool supportsTableLogging() const override;

    /**
     * We can support cross-shard transactions.
     */
    bool supportsCrossShardTransactions() const override;
};

}  // namespace mongo::rss
