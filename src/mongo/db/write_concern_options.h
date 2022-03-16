/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/db/read_write_concern_provenance.h"

namespace mongo {

class Status;

using WTags = StringMap<int64_t>;
using WriteConcernW = stdx::variant<std::string, std::int64_t, WTags>;

struct WriteConcernOptions {
public:
    enum class SyncMode { UNSET, NONE, FSYNC, JOURNAL };

    // This specifies the condition to check to satisfy given tags.
    // Users can only provide OpTime condition, the others are used internally.
    enum class CheckCondition { OpTime, Config };

    static constexpr Milliseconds kNoTimeout{0};
    static constexpr Milliseconds kNoWaiting{-1};

    static const BSONObj Default;
    static const BSONObj Acknowledged;
    static const BSONObj Unacknowledged;
    static const BSONObj Majority;
    static const BSONObj kInternalWriteDefault;

    static constexpr StringData kWriteConcernField = "writeConcern"_sd;
    static const char kMajority[];  // = "majority"

    static constexpr Seconds kWriteConcernTimeoutSystem{15};
    static constexpr Seconds kWriteConcernTimeoutMigration{30};
    static constexpr Seconds kWriteConcernTimeoutSharding{60};
    static constexpr Seconds kWriteConcernTimeoutUserCommand{60};

    WriteConcernOptions() = default;
    explicit WriteConcernOptions(int numNodes, SyncMode sync, Milliseconds timeout);
    explicit WriteConcernOptions(const std::string& mode, SyncMode sync, Milliseconds timeout);

    static StatusWith<WriteConcernOptions> parse(const BSONObj& obj);

    /**
     * Returns an instance of WriteConcernOptions from a BSONObj.
     *
     * uasserts() if the obj cannot be deserialized.
     */
    static WriteConcernOptions deserializerForIDL(const BSONObj& obj);

    /**
     * Attempts to extract a writeConcern from cmdObj.
     * Verifies that the writeConcern is of type Object (BSON type).
     */
    static StatusWith<WriteConcernOptions> extractWCFromCommand(const BSONObj& cmdObj);

    /**
     * Return true if the server needs to wait for other secondary nodes to satisfy this
     * write concern setting. Errs on the false positive for non-empty wMode.
     */
    bool needToWaitForOtherNodes() const;

    ReadWriteConcernProvenance& getProvenance() {
        return _provenance;
    }
    const ReadWriteConcernProvenance& getProvenance() const {
        return _provenance;
    }

    // Returns the BSON representation of this object.
    // Warning: does not return the same object passed on the last parse() call.
    BSONObj toBSON() const;

    bool operator==(const WriteConcernOptions& other) const;

    bool operator!=(const WriteConcernOptions& other) const {
        return !operator==(other);
    }

    /**
     * Return true if the default write concern is being used.
     *      - Default constructed WC (w:1) is being used.
     *      - Implicit default majority WC is being used.
     */
    bool isImplicitDefaultWriteConcern() const {
        return usedDefaultConstructedWC || _provenance.isImplicitDefault();
    }

    bool hasCustomWriteMode() const {
        return stdx::holds_alternative<std::string>(w) &&
            stdx::get<std::string>(w) != WriteConcernOptions::kMajority;
    }

    /**
     * Returns whether this write concern's w parameter is the number 0.
     */
    bool isUnacknowledged() const {
        return stdx::holds_alternative<int64_t>(w) && stdx::get<int64_t>(w) < 1;
    }

    /**
     * Returns whether this write concern's w parameter is the string "majority".
     */
    bool isMajority() const {
        return stdx::holds_alternative<std::string>(w) && stdx::get<std::string>(w) == kMajority;
    }

    // The w parameter for this write concern.
    WriteConcernW w{1};
    // Corresponds to the `j` or `fsync` parameters for write concern.
    SyncMode syncMode{SyncMode::UNSET};
    // Timeout in milliseconds.
    Milliseconds wTimeout{kNoTimeout};
    // Deadline. If this is set to something other than Date_t::max(), this takes precedence over
    // wTimeout.
    Date_t wDeadline{Date_t::max()};

    // True if the default constructed WC ({w:1}) was used.
    //      - Implicit default WC when value of w is {w:1}.
    //      - Internal commands set empty WC ({writeConcern: {}}), then default constructed WC (w:1)
    //        is used.
    // False otherwise:
    //      - Implicit default WC when value of w is {w:"majority"}.
    //      - Cluster-wide WC.
    //          - with (w) value set, for example ({writeConcern: {w:1}}).
    //          - without (w) value set, for example ({writeConcern: {j: true}}).
    //      - Client-supplied WC.
    //          - with (w) value set, for example ({writeConcern: {w:1}}).
    //          - without (w) value set, for example ({writeConcern: {j: true}}).
    bool usedDefaultConstructedWC{true};

    // Used only for tracking opWriteConcernCounters metric.
    // True if the (w) value of the write concern used is not set explicitly by client:
    //      - Default constructed WC ({w:1})
    //      - Implicit default majority WC.
    //      - Cluster-wide WC.
    //          - with (w) value set, for example ({writeConcern: {w:1}}).
    //          - without (w) value set, for example ({writeConcern: {j: true}}).
    //      - Client-supplied WC without (w) value set, for example ({writeConcern: {j: true}}).
    //      - Internal commands set empty WC ({writeConcern: {}}).
    bool notExplicitWValue{true};

    CheckCondition checkCondition{CheckCondition::OpTime};

private:
    ReadWriteConcernProvenance _provenance;
};

// Helpers for IDL parsing
WriteConcernW deserializeWriteConcernW(BSONElement wEl);
void serializeWriteConcernW(const WriteConcernW& w, StringData fieldName, BSONObjBuilder* builder);
std::int64_t parseWTimeoutFromBSON(BSONElement element);

}  // namespace mongo
