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

#include "mongo/platform/basic.h"

#include "mongo/db/write_concern_options.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/util/str.h"

namespace mongo {

using std::string;

namespace {

/**
 * Controls how much a client cares about writes and serves as initializer for the pre-defined
 * write concern options.
 *
 * Default is NORMAL.
 */
enum WriteConcern { W_NONE = 0, W_NORMAL = 1 };

constexpr StringData kJFieldName = "j"_sd;
constexpr StringData kFSyncFieldName = "fsync"_sd;
constexpr StringData kWFieldName = "w"_sd;
constexpr StringData kWTimeoutFieldName = "wtimeout"_sd;
constexpr StringData kGetLastErrorFieldName = "getLastError"_sd;
constexpr StringData kWOpTimeFieldName = "wOpTime"_sd;
constexpr StringData kWElectionIdFieldName = "wElectionId"_sd;

}  // namespace

constexpr int WriteConcernOptions::kNoTimeout;
constexpr int WriteConcernOptions::kNoWaiting;

constexpr StringData WriteConcernOptions::kWriteConcernField;
const char WriteConcernOptions::kMajority[] = "majority";

const BSONObj WriteConcernOptions::Default = BSONObj();
const BSONObj WriteConcernOptions::Acknowledged(BSON("w" << W_NORMAL));
const BSONObj WriteConcernOptions::Unacknowledged(BSON("w" << W_NONE));
const BSONObj WriteConcernOptions::Majority(BSON("w" << WriteConcernOptions::kMajority));

// The "kImplicitDefault" write concern, used by internal operations, is deliberately empty (no
// 'w' or 'wtimeout' specified). This allows internal operations to specify a write concern, while
// still allowing it to be either w:1 or automatically upconverted to w:majority on configsvrs.
const BSONObj WriteConcernOptions::kImplicitDefault;

constexpr Seconds WriteConcernOptions::kWriteConcernTimeoutSystem;
constexpr Seconds WriteConcernOptions::kWriteConcernTimeoutMigration;
constexpr Seconds WriteConcernOptions::kWriteConcernTimeoutSharding;
constexpr Seconds WriteConcernOptions::kWriteConcernTimeoutUserCommand;


WriteConcernOptions::WriteConcernOptions(int numNodes, SyncMode sync, int timeout)
    : WriteConcernOptions(numNodes, sync, Milliseconds(timeout)) {}

WriteConcernOptions::WriteConcernOptions(const std::string& mode, SyncMode sync, int timeout)
    : WriteConcernOptions(mode, sync, Milliseconds(timeout)) {}

WriteConcernOptions::WriteConcernOptions(int numNodes, SyncMode sync, Milliseconds timeout)
    : syncMode(sync), wNumNodes(numNodes), wTimeout(durationCount<Milliseconds>(timeout)) {}

WriteConcernOptions::WriteConcernOptions(const std::string& mode,
                                         SyncMode sync,
                                         Milliseconds timeout)
    : syncMode(sync), wNumNodes(0), wMode(mode), wTimeout(durationCount<Milliseconds>(timeout)) {}

StatusWith<WriteConcernOptions> WriteConcernOptions::parse(const BSONObj& obj) {
    if (obj.isEmpty()) {
        return Status(ErrorCodes::FailedToParse, "write concern object cannot be empty");
    }

    BSONElement jEl;
    BSONElement fsyncEl;
    BSONElement wEl;
    int wTimeout = 0;

    WriteConcernOptions writeConcern;

    for (auto e : obj) {
        const auto fieldName = e.fieldNameStringData();
        if (fieldName == kJFieldName) {
            jEl = e;
            if (!jEl.isNumber() && jEl.type() != Bool) {
                return Status(ErrorCodes::FailedToParse, "j must be numeric or a boolean value");
            }
            writeConcern.usedDefault = false;
        } else if (fieldName == kFSyncFieldName) {
            fsyncEl = e;
            if (!fsyncEl.isNumber() && fsyncEl.type() != Bool) {
                return Status(ErrorCodes::FailedToParse,
                              "fsync must be numeric or a boolean value");
            }
            writeConcern.usedDefault = false;
        } else if (fieldName == kWFieldName) {
            wEl = e;
            writeConcern.usedDefault = false;
        } else if (fieldName == kWTimeoutFieldName) {
            wTimeout = e.safeNumberLong();
            writeConcern.usedDefault = false;
        } else if (fieldName == kWElectionIdFieldName) {
            // Ignore.
        } else if (fieldName == kWOpTimeFieldName) {
            // Ignore.
        } else if (fieldName.equalCaseInsensitive(kGetLastErrorFieldName)) {
            // Ignore GLE field.
        } else if (fieldName == ReadWriteConcernProvenance::kSourceFieldName) {
            try {
                writeConcern._provenance = ReadWriteConcernProvenance::parse(
                    IDLParserErrorContext("WriteConcernOptions::parse"), obj);
            } catch (const DBException&) {
                return exceptionToStatus();
            }
        } else {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "unrecognized write concern field: " << fieldName);
        }
    }

    const bool j = jEl.trueValue();
    const bool fsync = fsyncEl.trueValue();
    if (j && fsync) {
        return Status(ErrorCodes::FailedToParse, "fsync and j options cannot be used together");
    }

    if (j) {
        writeConcern.syncMode = SyncMode::JOURNAL;
    } else if (fsync) {
        writeConcern.syncMode = SyncMode::FSYNC;
    } else if (!jEl.eoo()) {
        writeConcern.syncMode = SyncMode::NONE;
    }

    if (wEl.isNumber()) {
        auto wNumNodes = wEl.safeNumberLong();
        if (wNumNodes < 0 ||
            wNumNodes > static_cast<decltype(wNumNodes)>(repl::ReplSetConfig::kMaxMembers)) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "w has to be a non-negative number and not greater than "
                                        << repl::ReplSetConfig::kMaxMembers);
        }
        writeConcern.wNumNodes = static_cast<decltype(writeConcern.wNumNodes)>(wNumNodes);
        writeConcern.usedDefaultW = false;
    } else if (wEl.type() == String) {
        writeConcern.wNumNodes = 0;
        writeConcern.wMode = wEl.valuestrsafe();
        writeConcern.usedDefaultW = false;
    } else if (wEl.eoo() || wEl.type() == jstNULL || wEl.type() == Undefined) {
        writeConcern.wNumNodes = 1;
    } else {
        return Status(ErrorCodes::FailedToParse, "w has to be a number or a string");
    }

    writeConcern.wTimeout = wTimeout;

    return writeConcern;
}

WriteConcernOptions WriteConcernOptions::deserializerForIDL(const BSONObj& obj) {
    if (!obj.isEmpty()) {
        return uassertStatusOK(parse(obj));
    }
    return WriteConcernOptions();
}

StatusWith<WriteConcernOptions> WriteConcernOptions::extractWCFromCommand(const BSONObj& cmdObj) {
    WriteConcernOptions writeConcern;

    // Return the default write concern if no write concern is provided. We check for the existence
    // of the write concern field up front in order to avoid the expense of constructing an error
    // status in bsonExtractTypedField() below.
    if (!cmdObj.hasField(kWriteConcernField)) {
        return writeConcern;
    }

    BSONElement writeConcernElement;
    Status wcStatus =
        bsonExtractTypedField(cmdObj, kWriteConcernField, Object, &writeConcernElement);
    if (!wcStatus.isOK()) {
        return wcStatus;
    }

    BSONObj writeConcernObj = writeConcernElement.Obj();
    // Empty write concern is interpreted to default.
    if (writeConcernObj.isEmpty()) {
        return writeConcern;
    }

    auto sw = parse(writeConcernObj);
    if (!sw.isOK()) {
        return sw.getStatus();
    }
    writeConcern = sw.getValue();
    writeConcern.usedDefault = false;
    return writeConcern;
}

BSONObj WriteConcernOptions::toBSON() const {
    BSONObjBuilder builder;

    if (wMode.empty()) {
        builder.append("w", wNumNodes);
    } else {
        builder.append("w", wMode);
    }

    if (syncMode == SyncMode::FSYNC) {
        builder.append("fsync", true);
    } else if (syncMode == SyncMode::JOURNAL) {
        builder.append("j", true);
    } else if (syncMode == SyncMode::NONE) {
        builder.append("j", false);
    }

    builder.append("wtimeout", wTimeout);

    _provenance.serialize(&builder);

    return builder.obj();
}

bool WriteConcernOptions::needToWaitForOtherNodes() const {
    return !wMode.empty() || wNumNodes > 1;
}

bool WriteConcernOptions::operator==(const WriteConcernOptions& other) const {
    return syncMode == other.syncMode && wMode == other.wMode && wNumNodes == other.wNumNodes &&
        wDeadline == other.wDeadline && wTimeout == other.wTimeout &&
        _provenance == other._provenance;
}

}  // namespace mongo
