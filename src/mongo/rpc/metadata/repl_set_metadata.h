// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>

#include <boost/optional.hpp>

namespace mongo {

class BSONObj;
class BSONObjBuilder;

namespace [[MONGO_MOD_PUBLIC]] rpc {

extern const char kReplSetMetadataFieldName[];

/**
 * Represents the metadata information for $replData.
 */
class ReplSetMetadata {
public:
    ReplSetMetadata() = default;
    ReplSetMetadata(std::int64_t term,
                    repl::OpTimeAndWallTime committedOpTime,
                    repl::OpTime visibleOpTime,
                    std::int64_t configVersion,
                    std::int64_t configTerm,
                    OID id,
                    int currentSyncSourceIndex,
                    bool isPrimary);
    explicit ReplSetMetadata(const ReplSetMetadata&) = default;
    ReplSetMetadata(ReplSetMetadata&&) = default;
    ReplSetMetadata& operator=(const ReplSetMetadata&) = delete;
    ReplSetMetadata& operator=(ReplSetMetadata&&) = default;

    /**
     * format:
     * {
     *     term: 0,
     *     lastOpCommitted: {ts: Timestamp(0, 0), term: 0},
     *     lastOpVisible: {ts: Timestamp(0, 0), term: 0},
     *     configVersion: 0,
     *     replicaSetId: ObjectId("..."), // Only present in certain versions and above.
     *     primaryIndex: 0,
     *     syncSourceIndex: 0,
     *     isPrimary: false // 4.4 and later
     * }
     */
    static StatusWith<ReplSetMetadata> readFromMetadata(const BSONObj& doc);
    Status writeToMetadata(BSONObjBuilder* builder) const;

    /**
     * Helpers to carry ONLY the replication term in $replData, without the rest of ReplSetMetadata,
     * for callers that need to convey the term without assembling or parsing a full
     * ReplSetMetadata. appendTermOnly writes `$replData: {term: <term>}` into 'builder'.
     * readTermOnly returns the term if 'reply' carries $replData.term, else boost::none; it does
     * not require a full ReplSetMetadata, so it is safe on the partial object appendTermOnly
     * produces.
     * TODO SERVER-130332: Remove these helpers.
     */
    static void appendTermOnly(BSONObjBuilder* builder, std::int64_t term);
    static boost::optional<std::int64_t> readTermOnly(const BSONObj& reply);

    /**
     * Returns the OpTime of the most recent operation with which the client interacted.
     */
    repl::OpTime getLastOpVisible() const {
        return _lastOpVisible;
    }

    /**
     * Returns the OpTime of the most recently committed op of which the sender was aware.
     */
    repl::OpTimeAndWallTime getLastOpCommitted() const {
        return _lastOpCommitted;
    }

    /**
     * Returns the ReplSetConfig version number of the sender.
     */
    std::int64_t getConfigVersion() const {
        return _configVersion;
    }

    /**
     * Returns the ReplSetConfig term number of the sender.
     */
    std::int64_t getConfigTerm() const {
        return _configTerm;
    }

    /**
     * Returns true if the sender has a replica set ID.
     */
    bool hasReplicaSetId() const {
        return _replicaSetId.isSet();
    }

    /**
     * Returns the replica set ID of the sender.
     */
    OID getReplicaSetId() const {
        return _replicaSetId;
    }

    /**
     * Returns the index of the sync source of the sender.
     * Returns -1 if it has no sync source.
     */
    int getSyncSourceIndex() const {
        return _currentSyncSourceIndex;
    }

    /**
     * Returns true if the sender is primary.
     */
    bool getIsPrimary() const {
        return _isPrimary;
    }

    /**
     * Returns the current term from the perspective of the sender.
     */
    std::int64_t getTerm() const {
        return _currentTerm;
    }

    /**
     * Returns a stringified version of the metadata.
     */
    std::string toString() const;

private:
    repl::OpTimeAndWallTime _lastOpCommitted;
    repl::OpTime _lastOpVisible;
    std::int64_t _currentTerm = -1;
    std::int64_t _configVersion = -1;
    std::int64_t _configTerm = repl::OpTime::kUninitializedTerm;
    OID _replicaSetId;
    int _currentSyncSourceIndex = -1;
    bool _isPrimary = false;
};

}  // namespace rpc
}  // namespace mongo
