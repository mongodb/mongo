/*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/write_concern_options.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

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

const int WriteConcernOptions::kNoTimeout(0);
const int WriteConcernOptions::kNoWaiting(-1);

const char WriteConcernOptions::kMajority[] = "majority";

const BSONObj WriteConcernOptions::Default = BSONObj();
const BSONObj WriteConcernOptions::Acknowledged(BSON("w" << W_NORMAL));
const BSONObj WriteConcernOptions::Unacknowledged(BSON("w" << W_NONE));
const BSONObj WriteConcernOptions::Majority(BSON("w" << WriteConcernOptions::kMajority));

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

Status WriteConcernOptions::parse(const BSONObj& obj) {
    reset();
    if (obj.isEmpty()) {
        return Status(ErrorCodes::FailedToParse, "write concern object cannot be empty");
    }

    BSONElement jEl;
    BSONElement fsyncEl;
    BSONElement wEl;


    for (auto e : obj) {
        const auto fieldName = e.fieldNameStringData();
        if (fieldName == kJFieldName) {
            jEl = e;
            if (!jEl.isNumber() && jEl.type() != Bool) {
                return Status(ErrorCodes::FailedToParse, "j must be numeric or a boolean value");
            }
        } else if (fieldName == kFSyncFieldName) {
            fsyncEl = e;
            if (!fsyncEl.isNumber() && fsyncEl.type() != Bool) {
                return Status(ErrorCodes::FailedToParse,
                              "fsync must be numeric or a boolean value");
            }
        } else if (fieldName == kWFieldName) {
            wEl = e;
        } else if (fieldName == kWTimeoutFieldName) {
            wTimeout = e.numberInt();
        } else if (fieldName == kWElectionIdFieldName) {
            // Ignore.
        } else if (fieldName == kWOpTimeFieldName) {
            // Ignore.
        } else if (fieldName.equalCaseInsensitive(kGetLastErrorFieldName)) {
            // Ignore GLE field.
        } else {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "unrecognized write concern field: " << fieldName);
        }
    }

    const bool j = jEl.trueValue();
    const bool fsync = fsyncEl.trueValue();

    if (j && fsync)
        return Status(ErrorCodes::FailedToParse, "fsync and j options cannot be used together");

    if (j) {
        syncMode = SyncMode::JOURNAL;
    } else if (fsync) {
        syncMode = SyncMode::FSYNC;
    } else if (!jEl.eoo()) {
        syncMode = SyncMode::NONE;
    }

    if (wEl.isNumber()) {
        wNumNodes = wEl.numberInt();
    } else if (wEl.type() == String) {
        wMode = wEl.valuestrsafe();
    } else if (wEl.eoo() || wEl.type() == jstNULL || wEl.type() == Undefined) {
        wNumNodes = 1;
    } else {
        return Status(ErrorCodes::FailedToParse, "w has to be a number or a string");
    }

    return Status::OK();
}

StatusWith<WriteConcernOptions> WriteConcernOptions::extractWCFromCommand(
    const BSONObj& cmdObj, const std::string& dbName, const WriteConcernOptions& defaultWC) {
    WriteConcernOptions writeConcern = defaultWC;
    writeConcern.usedDefault = true;
    if (writeConcern.wNumNodes == 0 && writeConcern.wMode.empty()) {
        writeConcern.wNumNodes = 1;
    }

    BSONElement writeConcernElement;
    Status wcStatus = bsonExtractTypedField(cmdObj, "writeConcern", Object, &writeConcernElement);
    if (!wcStatus.isOK()) {
        if (wcStatus == ErrorCodes::NoSuchKey) {
            // Return default write concern if no write concern is given.
            return writeConcern;
        }
        return wcStatus;
    }

    BSONObj writeConcernObj = writeConcernElement.Obj();
    // Empty write concern is interpreted to default.
    if (writeConcernObj.isEmpty()) {
        return writeConcern;
    }

    wcStatus = writeConcern.parse(writeConcernObj);
    writeConcern.usedDefault = false;
    if (!wcStatus.isOK()) {
        return wcStatus;
    }

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

    return builder.obj();
}

bool WriteConcernOptions::shouldWaitForOtherNodes() const {
    return !wMode.empty() || wNumNodes > 1;
}

bool WriteConcernOptions::validForConfigServers() const {
    return wMode == kMajority;
}

}  // namespace mongo
