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

#include <type_traits>

#include "mongo/db/write_concern_options.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/write_concern_options_gen.h"
#include "mongo/util/str.h"

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

constexpr Milliseconds WriteConcernOptions::kNoTimeout;
constexpr Milliseconds WriteConcernOptions::kNoWaiting;

constexpr StringData WriteConcernOptions::kWriteConcernField;
const char WriteConcernOptions::kMajority[] = "majority";

const BSONObj WriteConcernOptions::Default = BSONObj();
const BSONObj WriteConcernOptions::Acknowledged(BSON("w" << W_NORMAL));
const BSONObj WriteConcernOptions::Unacknowledged(BSON("w" << W_NONE));
const BSONObj WriteConcernOptions::Majority(BSON("w" << WriteConcernOptions::kMajority));

// The "kInternalWriteDefault" write concern, used by internal operations, is deliberately empty (no
// 'w' or 'wtimeout' specified). This allows internal operations to specify a write concern, while
// still allowing it to be either w:1 or automatically upconverted to w:majority on configsvrs.
const BSONObj WriteConcernOptions::kInternalWriteDefault;

constexpr Seconds WriteConcernOptions::kWriteConcernTimeoutSystem;
constexpr Seconds WriteConcernOptions::kWriteConcernTimeoutMigration;
constexpr Seconds WriteConcernOptions::kWriteConcernTimeoutSharding;
constexpr Seconds WriteConcernOptions::kWriteConcernTimeoutUserCommand;

WriteConcernOptions::WriteConcernOptions(int numNodes, SyncMode sync, Milliseconds timeout)
    : w{numNodes},
      syncMode{sync},
      wTimeout(durationCount<Milliseconds>(timeout)),
      usedDefaultConstructedWC{false},
      notExplicitWValue{false} {}

WriteConcernOptions::WriteConcernOptions(const std::string& mode,
                                         SyncMode sync,
                                         Milliseconds timeout)
    : w{mode},
      syncMode{sync},
      wTimeout(durationCount<Milliseconds>(timeout)),
      usedDefaultConstructedWC{false},
      notExplicitWValue{false} {}

StatusWith<WriteConcernOptions> WriteConcernOptions::parse(const BSONObj& obj) try {
    if (obj.isEmpty()) {
        return Status(ErrorCodes::FailedToParse, "write concern object cannot be empty");
    }

    auto writeConcernIdl = WriteConcernIdl::parse(IDLParserContext{"WriteConcernOptions"}, obj);
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
        bsonExtractTypedField(cmdObj, kWriteConcernField, Object, &writeConcernElement);
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

WriteConcernW deserializeWriteConcernW(BSONElement wEl) {
    if (wEl.isNumber()) {
        auto wNum = wEl.safeNumberLong();
        if (wNum < 0 || wNum > static_cast<long long>(repl::ReplSetConfig::kMaxMembers)) {
            uasserted(ErrorCodes::FailedToParse,
                      "w has to be a non-negative number and not greater than {}; found: {}"_format(
                          repl::ReplSetConfig::kMaxMembers, wNum));
        }

        return WriteConcernW{wNum};
    } else if (wEl.type() == BSONType::String) {
        return WriteConcernW{wEl.str()};
    } else if (wEl.type() == BSONType::Object) {
        auto wTags = wEl.Obj();
        uassert(ErrorCodes::FailedToParse, "tagged write concern requires tags", !wTags.isEmpty());

        WTags tags;
        for (auto&& e : wTags) {
            uassert(
                ErrorCodes::FailedToParse,
                "tags must be a single level document with only number values; found: {}"_format(
                    e.toString()),
                e.isNumber());

            tags.try_emplace(e.fieldName(), e.safeNumberInt());
        }

        return WriteConcernW{std::move(tags)};
    } else if (wEl.eoo() || wEl.type() == BSONType::jstNULL || wEl.type() == BSONType::Undefined) {
        return WriteConcernW{};
    }
    uasserted(ErrorCodes::FailedToParse,
              "w has to be a number, string, or object; found: {}"_format(typeName(wEl.type())));
}

void serializeWriteConcernW(const WriteConcernW& w, StringData fieldName, BSONObjBuilder* builder) {
    stdx::visit(OverloadedVisitor{[&](int64_t wNumNodes) {
                                      builder->appendNumber(fieldName,
                                                            static_cast<long long>(wNumNodes));
                                  },
                                  [&](std::string wMode) { builder->append(fieldName, wMode); },
                                  [&](WTags wTags) { builder->append(fieldName, wTags); }},
                w);
}

std::int64_t parseWTimeoutFromBSON(BSONElement element) {
    constexpr std::array<mongo::BSONType, 4> validTypes{
        NumberLong, NumberInt, NumberDecimal, NumberDouble};
    bool isValidType = std::any_of(
        validTypes.begin(), validTypes.end(), [&](auto type) { return element.type() == type; });
    return isValidType ? element.safeNumberLong() : 0;
}

BSONObj WriteConcernOptions::toBSON() const {
    BSONObjBuilder builder;
    serializeWriteConcernW(w, "w", &builder);

    if (syncMode == SyncMode::FSYNC) {
        builder.append("fsync", true);
    } else if (syncMode == SyncMode::JOURNAL) {
        builder.append("j", true);
    } else if (syncMode == SyncMode::NONE) {
        builder.append("j", false);
    }

    // Historically we have serialized this as a int32_t, even though it is defined as an
    // int64_t in our IDL format.
    builder.append("wtimeout", static_cast<int32_t>(durationCount<Milliseconds>(wTimeout)));

    _provenance.serialize(&builder);

    return builder.obj();
}

bool WriteConcernOptions::needToWaitForOtherNodes() const {
    return stdx::holds_alternative<std::string>(w) || stdx::holds_alternative<WTags>(w) ||
        (stdx::holds_alternative<std::int64_t>(w) && stdx::get<std::int64_t>(w) > 1);
}

bool WriteConcernOptions::operator==(const WriteConcernOptions& other) const {
    return w == other.w && syncMode == other.syncMode && wDeadline == other.wDeadline &&
        wTimeout == other.wTimeout && _provenance == other._provenance;
}

}  // namespace mongo
