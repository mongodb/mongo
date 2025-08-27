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

#include "mongo/db/write_concern_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/write_concern_options_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

namespace mongo {
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

constexpr Milliseconds WriteConcernOptions::Timeout::kNoTimeoutVal;
constexpr Milliseconds WriteConcernOptions::Timeout::kNoWaitingVal;

constexpr WriteConcernOptions::Timeout WriteConcernOptions::kNoTimeout(
    WriteConcernOptions::Timeout::kNoTimeoutVal);
constexpr WriteConcernOptions::Timeout WriteConcernOptions::kNoWaiting(
    WriteConcernOptions::Timeout::kNoWaitingVal);

constexpr StringData WriteConcernOptions::kWriteConcernField;
const char WriteConcernOptions::kMajority[] = "majority";

const BSONObj WriteConcernOptions::Default = BSONObj();
const BSONObj WriteConcernOptions::Acknowledged(BSON("w" << W_NORMAL));
const BSONObj WriteConcernOptions::Unacknowledged(BSON("w" << W_NONE));
const BSONObj WriteConcernOptions::Majority(BSON("w" << WriteConcernOptions::kMajority));

// The "kInternalWriteDefault" write concern used by internal operations, is deliberately empty (no
// 'w' or 'wtimeout' specified). We require that all internal operations explicitly specify a write
// concern, so "kInternalWriteDefault" allows internal operations to explicitly specify a write
// concern, without counting as a "client-supplied write concern" and instead still using the
// "default constructed WC" ({w:1})
const BSONObj WriteConcernOptions::kInternalWriteDefault;

WriteConcernOptions::WriteConcernOptions(int numNodes, SyncMode sync, Milliseconds timeout)
    : w{numNodes},
      syncMode{sync},
      wTimeout(timeout),
      usedDefaultConstructedWC{false},
      notExplicitWValue{false} {}

WriteConcernOptions::WriteConcernOptions(const std::string& mode,
                                         SyncMode sync,
                                         Milliseconds timeout)
    : w{mode},
      syncMode{sync},
      wTimeout(timeout),
      usedDefaultConstructedWC{false},
      notExplicitWValue{false} {}

WriteConcernOptions::WriteConcernOptions(int numNodes, SyncMode sync, Timeout timeout)
    : w{numNodes},
      syncMode{sync},
      wTimeout(timeout),
      usedDefaultConstructedWC{false},
      notExplicitWValue{false} {}

WriteConcernOptions::WriteConcernOptions(const std::string& mode, SyncMode sync, Timeout timeout)
    : w{mode},
      syncMode{sync},
      wTimeout(timeout),
      usedDefaultConstructedWC{false},
      notExplicitWValue{false} {}

StatusWith<WriteConcernOptions> WriteConcernOptions::parse(const BSONObj& obj) try {
    if (obj.isEmpty()) {
        return Status(ErrorCodes::FailedToParse, "write concern object cannot be empty");
    }

    auto writeConcernIdl = WriteConcernIdl::parse(obj, IDLParserContext{"WriteConcernOptions"});
    auto parsedW = writeConcernIdl.getWriteConcernW();

    WriteConcernOptions writeConcern;
    writeConcern.usedDefaultConstructedWC = !parsedW && !writeConcernIdl.getJ() &&
        !writeConcernIdl.getFsync() && writeConcernIdl.getWtimeout() == 0;

    if (parsedW) {
        writeConcern.notExplicitWValue = false;
        writeConcern.w = *parsedW;
    }

    auto j = writeConcernIdl.getJ();
    auto fsync = writeConcernIdl.getFsync();
    if (j && fsync && *j && *fsync) {
        // If j and fsync are both set to true
        return Status{ErrorCodes::FailedToParse, "fsync and j options cannot be used together"};
    }

    if (j && *j) {
        writeConcern.syncMode = SyncMode::JOURNAL;
    } else if (fsync && *fsync) {
        writeConcern.syncMode = SyncMode::FSYNC;
    } else if (j) {
        // j has been set to false
        writeConcern.syncMode = SyncMode::NONE;
    }

    writeConcern.wTimeout = Milliseconds{writeConcernIdl.getWtimeout()};
    if (auto source = writeConcernIdl.getSource()) {
        writeConcern._provenance = ReadWriteConcernProvenance(*source);
    }

    return writeConcern;
} catch (const DBException& ex) {
    return ex.toStatus();
}

WriteConcernOptions WriteConcernOptions::deserializerForIDL(const BSONObj& obj) {
    if (!obj.isEmpty()) {
        return uassertStatusOK(parse(obj));
    }
    return WriteConcernOptions();
}

StatusWith<WriteConcernOptions> WriteConcernOptions::extractWCFromCommand(const BSONObj& cmdObj) {
    // If no write concern is provided from the command, return the default write concern
    // ({w: 1, wtimeout: 0}). If the default write concern is returned, it will be overriden in
    // extractWriteConcern by the cluster-wide write concern or the implicit default write concern.
    // We check for the existence of the write concern field up front in order to avoid the expense
    // of constructing an error status in bsonExtractTypedField() below.
    if (!cmdObj.hasField(kWriteConcernField)) {
        return WriteConcernOptions();
    }

    BSONElement writeConcernElement;
    Status wcStatus =
        bsonExtractTypedField(cmdObj, kWriteConcernField, BSONType::object, &writeConcernElement);
    if (!wcStatus.isOK()) {
        return wcStatus;
    }

    BSONObj writeConcernObj = writeConcernElement.Obj();
    // Empty write concern is interpreted to default.
    if (writeConcernObj.isEmpty()) {
        return WriteConcernOptions();
    }

    return parse(writeConcernObj);
}

WriteConcernIdl WriteConcernOptions::toWriteConcernIdl() const {
    WriteConcernIdl idl;

    idl.setWriteConcernW(w);

    if (syncMode == SyncMode::FSYNC) {
        idl.setFsync(true);
    } else if (syncMode == SyncMode::JOURNAL) {
        idl.setJ(true);
    } else if (syncMode == SyncMode::NONE) {
        idl.setJ(false);
    }

    wTimeout.addToIDL(&idl);
    idl.setSource(_provenance.getSource());

    return idl;
}

BSONObj WriteConcernOptions::toBSON() const {
    return toWriteConcernIdl().toBSON();
}

bool WriteConcernOptions::needToWaitForOtherNodes() const {
    return holds_alternative<std::string>(w) || holds_alternative<WTags>(w) ||
        (holds_alternative<std::int64_t>(w) && get<std::int64_t>(w) > 1);
}

bool WriteConcernOptions::operator==(const WriteConcernOptions& other) const {
    return w == other.w && syncMode == other.syncMode && wTimeout == other.wTimeout &&
        _provenance == other._provenance;
}

}  // namespace mongo
