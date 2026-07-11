// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_provenance.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_gen.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
using namespace std::literals::string_view_literals;

class [[MONGO_MOD_PUBLIC]] ReadConcernArgs {
public:
    static constexpr std::string_view kReadConcernFieldName = "readConcern"sv;
    static constexpr std::string_view kAfterOpTimeFieldName = ReadConcernIdl::kAfterOpTimeFieldName;
    static constexpr std::string_view kAfterClusterTimeFieldName =
        ReadConcernIdl::kAfterClusterTimeFieldName;
    static constexpr std::string_view kAtClusterTimeFieldName =
        ReadConcernIdl::kAtClusterTimeFieldName;
    static constexpr std::string_view kLevelFieldName = ReadConcernIdl::kLevelFieldName;
    static constexpr std::string_view kAllowTransactionTableSnapshot =
        ReadConcernIdl::kAllowTransactionTableSnapshotFieldName;
    static constexpr std::string_view kWaitLastStableRecoveryTimestamp =
        ReadConcernIdl::kWaitLastStableRecoveryTimestampFieldName;

    static const ReadConcernArgs kImplicitDefault;
    static const ReadConcernArgs kLocal;
    static const ReadConcernArgs kMajority;
    static const ReadConcernArgs kAvailable;
    static const ReadConcernArgs kLinearizable;
    static const ReadConcernArgs kSnapshot;

    /**
     * Represents the internal mechanism an operation uses to satisfy 'majority' read concern.
     */
    enum class MajorityReadMechanism {
        // The storage engine will read from a historical, majority committed snapshot of data. This
        // is the default mechanism.
        kMajoritySnapshot,

        // A mechanism to be used when the storage engine does not support reading from a historical
        // snapshot. A query will read from a local (potentially uncommitted) snapshot, and, after
        // reading data, will block until it knows that data has become majority committed.
        kSpeculative
    };

    static ReadConcernArgs& get(OperationContext* opCtx);
    static const ReadConcernArgs& get(const OperationContext* opCtx);

    ReadConcernArgs();

    ReadConcernArgs(boost::optional<ReadConcernLevel> level);

    ReadConcernArgs(boost::optional<OpTime> opTime, boost::optional<ReadConcernLevel> level);

    ReadConcernArgs(boost::optional<LogicalTime> afterClusterTime,
                    boost::optional<ReadConcernLevel> level);

    static ReadConcernArgs snapshot(
        LogicalTime atClusterTime,
        boost::optional<bool> allowTransactionTableSnapshot = boost::none) {
        auto rc = ReadConcernArgs::kSnapshot;
        rc.setArgsAtClusterTimeForSnapshot(atClusterTime.asTimestamp());
        if (allowTransactionTableSnapshot) {
            rc._allowTransactionTableSnapshot = *allowTransactionTableSnapshot;
        }
        return rc;
    }

    static ReadConcernArgs snapshot(Timestamp atClusterTime) {
        auto rc = ReadConcernArgs::kSnapshot;
        rc.setArgsAtClusterTimeForSnapshot(atClusterTime);
        return rc;
    }

    /**
     * Format:
     * {
     *    find: "coll"
     *    filter: <Query Object>,
     *    readConcern: { // optional
     *      level: "[majority|local|linearizable|available|snapshot]",
     *      afterOpTime: { ts: <timestamp>, term: <NumberLong> },
     *      afterClusterTime: <timestamp>,
     *      atClusterTime: <timestamp>
     *    }
     * }
     */
    Status initialize(const BSONObj& cmdObj) {
        return initialize(cmdObj[kReadConcernFieldName]);
    }

    /**
     * Initializes the object from the readConcern element in a command object.
     * Use this if you are already iterating over the fields in the command object.
     * This method correctly handles missing BSONElements.
     */
    Status initialize(const BSONElement& readConcernElem);

    /**
     * Initializes the object by parsing the actual readConcern sub-object.
     */
    Status parse(const BSONObj& readConcernObj);

    /**
     * Initializes the object by parsing the IDL representation of the actual readConcern
     * sub-object.
     */
    Status parse(ReadConcernIdl inner);

    static ReadConcernArgs fromBSONThrows(const BSONObj& readConcernObj);
    static ReadConcernArgs fromIDLThrows(ReadConcernIdl readConcern);

    /**
     * Returns the mechanism to use for satisfying 'majority' read concern.
     *
     * Invalid to call unless the read concern level is 'kMajorityReadConcern'.
     */
    MajorityReadMechanism getMajorityReadMechanism() const;

    /**
     * Appends level, afterOpTime, and any other sub-fields in a 'readConcern' sub-object.
     */
    void appendInfo(BSONObjBuilder* builder) const;

    /**
     * Returns true if any of clusterTime, opTime or level arguments are set. Does not differentiate
     * between an unspecified read concern and an empty one (i.e. an empty BSON object).
     */
    bool isEmpty() const;

    /**
     * Returns true if this ReadConcernArgs represents a read concern that was actually specified.
     * If the RC was specified as an empty BSON object this will still be true (unlike isEmpty()).
     * False represents an absent or missing read concern, ie. one which wasn't present at all.
     */
    bool isSpecified() const {
        return _specified;
    }

    /**
     * Returns true if this ReadConcernArgs represents an implicit default read concern.
     */
    bool isImplicitDefault() const;

    /**
     *  Returns default kLocalReadConcern if _level is not set.
     */
    ReadConcernLevel getLevel() const {
        return _level.value_or(ReadConcernLevel::kLocalReadConcern);
    }

    /**
     * Checks whether _level is explicitly set.
     */
    bool hasLevel() const {
        return _level.has_value();
    }

    /**
     * Sets the read concern level (used to merge a default level into a partial RC).
     */
    void setLevel(ReadConcernLevel level) {
        _level = level;
    }

    /**
     * Returns the opTime. Deprecated: will be replaced with getArgsAfterClusterTime.
     */
    boost::optional<OpTime> getArgsOpTime() const {
        return _opTime;
    }

    boost::optional<LogicalTime> getArgsAfterClusterTime() const {
        return _afterClusterTime;
    }

    boost::optional<LogicalTime> getArgsAtClusterTime() const {
        return _atClusterTime;
    }

    /**
     * Returns a BSON object of the form:
     *
     * { readConcern: { level: "...",
     *                  afterClusterTime: Timestamp(...) } }
     */
    BSONObj toBSON() const;

    /**
     * Returns a BSON object of the form:
     *
     * { level: "...",
     *   afterClusterTime: Timestamp(...) }
     */
    BSONObj toBSONInner() const;
    std::string toString() const;

    /**
     * Returns the IDL-struct representation of this object's inner BSON serialized form.
     */
    ReadConcernIdl toReadConcernIdl() const;

    ReadWriteConcernProvenance& getProvenance() {
        return _provenance;
    }
    const ReadWriteConcernProvenance& getProvenance() const {
        return _provenance;
    }

    /**
     * Set atClusterTime, clear afterClusterTime. The BSON representation becomes
     * {level: "snapshot", atClusterTime: <ts>}.
     */
    void setArgsAtClusterTimeForSnapshot(Timestamp ts) {
        invariant(_level);
        invariant(_level == ReadConcernLevel::kSnapshotReadConcern);
        // Only overwrite a server-selected atClusterTime, not user-supplied.
        invariant(_atClusterTime.is_initialized() == _atClusterTimeSelected);
        _afterClusterTime = boost::none;
        _atClusterTime = LogicalTime(ts);
        _atClusterTimeSelected = true;
    }

    /**
     * Return whether an atClusterTime has been selected by the server for a snapshot read. This
     * function returns false if the atClusterTime was specified by the client.
     */
    bool wasAtClusterTimeSelected() const {
        return _atClusterTimeSelected;
    }

    bool allowTransactionTableSnapshot() const {
        return _allowTransactionTableSnapshot;
    }

    bool waitLastStableRecoveryTimestamp() const {
        return _waitLastStableRecoveryTimestamp;
    }

    void setWaitLastStableRecoveryTimestamp(bool wait) {
        _waitLastStableRecoveryTimestamp = wait;
    }

    /**
     * Computes the OpTime that this ReadConcernArgs intends to wait for.
     * Exactly one of getArgsAfterClusterTime(), getArgsAtClusterTime(), getArgsOpTime() must be
     * non-empty and must not be un-initialized.
     */
    OpTime getTargetOpTime() const;

    /**
     * Returns true if this ReadConcernArgs represents a majority committed read, and false
     * otherwise.
     */
    bool isMajorityCommittedRead(bool inMultiDocumentTransaction) const;

    /**
     * Verifies that this ReadConcernArgs instance represents a valid combination of readConcern
     * level and associated timing options.
     */
    Status validate() const;

private:
    /**
     * Appends level, afterOpTime, and the other "inner" fields of the read concern args.
     */
    void _appendInfoInner(BSONObjBuilder* builder) const;

    /**
     *  Read data after the OpTime of an operation on this replica set. Deprecated.
     *  The only user is for read-after-optime calls using the config server optime.
     */
    boost::optional<OpTime> _opTime;
    /**
     *  Read data after cluster-wide cluster time.
     */
    boost::optional<LogicalTime> _afterClusterTime;
    /**
     * Read data at a particular cluster time.
     */
    boost::optional<LogicalTime> _atClusterTime;
    boost::optional<ReadConcernLevel> _level;

    /**
     * The mechanism to use for satisfying 'majority' reads. Only meaningful if the read concern
     * level is 'majority'.
     */
    MajorityReadMechanism _majorityReadMechanism{MajorityReadMechanism::kMajoritySnapshot};

    /**
     * True indicates that a read concern has been specified (even if it might be empty), as
     * opposed to being absent or missing.
     */
    bool _specified;

    ReadWriteConcernProvenance _provenance;

    bool _atClusterTimeSelected = false;

    bool _allowTransactionTableSnapshot = false;

    bool _waitLastStableRecoveryTimestamp = false;
};

}  // namespace repl
}  // namespace mongo
