// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

class BSONObj;
class BSONObjBuilder;

namespace [[MONGO_MOD_PUBLIC]] rpc {

extern const char kOplogQueryMetadataFieldName[];

/**
 * Represents the metadata information for $oplogQueryData.
 */
class OplogQueryMetadata {
public:
    /**
     * Default primary index. Also used to indicate in metadata that there is no
     * primary.
     */
    static const int kNoPrimary = -1;

    OplogQueryMetadata() = default;
    OplogQueryMetadata(repl::OpTimeAndWallTime lastOpCommitted,
                       repl::OpTime lastOpApplied,
                       repl::OpTime lastOpWritten,
                       int rbid,
                       int currentPrimaryIndex,
                       int currentSyncSourceIndex,
                       std::string currentSyncSourceHost);
    explicit OplogQueryMetadata(const OplogQueryMetadata&) = default;
    OplogQueryMetadata(OplogQueryMetadata&&) = default;
    OplogQueryMetadata& operator=(const OplogQueryMetadata&) = delete;
    OplogQueryMetadata& operator=(OplogQueryMetadata&&) = default;

    /**
     * format:
     * {
     *     lastOpCommitted: {ts: Timestamp(0, 0), term: 0},
     *     lastCommittedWall: ISODate("2018-07-25T19:21:22.449Z")
     *     lastOpApplied: {ts: Timestamp(0, 0), term: 0},
     *     lastOpWritten: {ts: Timestamp(0, 0), term: 0},
     *     rbid: 0
     *     primaryIndex: 0,
     *     syncSourceIndex: 0
     * }
     */
    static StatusWith<OplogQueryMetadata> readFromMetadata(const BSONObj& doc);
    Status writeToMetadata(BSONObjBuilder* builder) const;

    /**
     * Returns the OpTime of the most recently committed op of which the sender was aware.
     */
    repl::OpTimeAndWallTime getLastOpCommitted() const {
        return _lastOpCommitted;
    }

    /**
     * Returns the OpTime of the most recent operation to be applied by the sender.
     */
    repl::OpTime getLastOpApplied() const {
        return _lastOpApplied;
    }

    /**
     * Returns the OpTime of the most recent operation to be written by the sender.
     */
    repl::OpTime getLastOpWritten() const {
        return _lastOpWritten;
    }

    /**
     * True if the sender thinks there is a primary.
     *
     * Note: the $oplogQueryData's primaryIndex isn't safe to use, we don't know which config
     * version it's from. All we can safely say is whether the sender thinks there's a primary.
     */
    bool hasPrimaryIndex() const {
        return _currentPrimaryIndex != kNoPrimary;
    }

    /**
     * Returns the index of the sync source of the sender.
     * Returns -1 if it has no sync source.
     */
    int getSyncSourceIndex() const {
        return _currentSyncSourceIndex;
    }

    /**
     * Returns the host of the sync source of the sender.
     * Returns empty string if it has no sync source.
     */
    std::string getSyncSourceHost() const {
        return _currentSyncSourceHost;
    }

    /**
     * Returns the current rbid of the sender.
     */
    int getRBID() const {
        return _rbid;
    }

    /**
     * Returns a stringified version of the metadata.
     */
    std::string toString() const;

private:
    repl::OpTimeAndWallTime _lastOpCommitted;
    repl::OpTime _lastOpApplied;
    repl::OpTime _lastOpWritten;
    int _rbid = -1;
    int _currentPrimaryIndex = kNoPrimary;
    int _currentSyncSourceIndex = -1;
    std::string _currentSyncSourceHost;
};

}  // namespace rpc
}  // namespace mongo
