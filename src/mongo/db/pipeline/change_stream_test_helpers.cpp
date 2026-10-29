// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_test_helpers.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::change_stream_test_helper {
using namespace std::literals::string_view_literals;

const UUID& testUuid() {
    static const UUID* uuid_gen = new UUID(UUID::gen());
    return *uuid_gen;
}

LogicalSessionFromClient testLsid() {
    // Required to avoid static initialization fiasco.
    static const UUID* uuid = new UUID(UUID::gen());
    LogicalSessionFromClient lsid{};
    lsid.setId(*uuid);
    lsid.setUid(SHA256Block{});
    return lsid;
}

Document makeResumeToken(Timestamp ts,
                         ImplicitValue uuid,
                         ImplicitValue docKeyOrOpDesc,
                         std::string_view operationType,
                         ResumeTokenData::FromInvalidate fromInvalidate,
                         size_t txnOpIndex) {
    static const std::set<std::string_view> kCrudOps = {
        "insert"sv, "update"sv, "replace"sv, "delete"sv};
    auto eventId = Value(Document{
        {"operationType", operationType},
        {kCrudOps.count(operationType) ? "documentKey" : "operationDescription", docKeyOrOpDesc}});
    return makeResumeTokenWithEventId(ts, uuid, eventId, fromInvalidate, txnOpIndex);
}

Document makeResumeTokenWithEventId(Timestamp ts,
                                    ImplicitValue uuid,
                                    ImplicitValue eventIdentifier,
                                    ResumeTokenData::FromInvalidate fromInvalidate,
                                    size_t txnOpIndex) {
    auto optionalUuid = uuid.missing() ? boost::none : boost::make_optional(uuid.getUuid());
    ResumeTokenData tokenData{ts,
                              ResumeTokenData::kDefaultTokenVersion,
                              txnOpIndex,
                              optionalUuid,
                              eventIdentifier,
                              fromInvalidate};
    return ResumeToken(tokenData).toDocument();
}

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj object,
                                boost::optional<UUID> uuid,
                                boost::optional<bool> fromMigrate,
                                boost::optional<BSONObj> object2,
                                boost::optional<repl::OpTime> opTime,
                                OperationSessionInfo sessionInfo,
                                boost::optional<repl::OpTime> prevOpTime,
                                boost::optional<repl::OpTime> preImageOpTime) {
    return {repl::DurableOplogEntry(opTime ? *opTime : kDefaultOpTime,  // optime
                                    opType,                             // opType
                                    nss,                                // namespace
                                    uuid,                               // uuid
                                    fromMigrate,                        // fromMigrate
                                    boost::none,                      // checkExistenceForDiffInsert
                                    boost::none,                      // versionContext
                                    repl::OplogEntry::kOplogVersion,  // version
                                    object,                           // o
                                    object2,                          // o2
                                    sessionInfo,                      // sessionInfo
                                    boost::none,                      // upsert
                                    Date_t(),                         // wall clock time
                                    {},                               // statement ids
                                    prevOpTime,  // optime of previous write within same transaction
                                    preImageOpTime,  // pre-image optime
                                    boost::none,     // post-image optime
                                    boost::none,     // ShardId of resharding recipient
                                    boost::none,     // _id
                                    boost::none)};   // needsRetryImage
}
}  // namespace mongo::change_stream_test_helper
