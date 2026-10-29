// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::change_stream_test_helper {
static const Timestamp kDefaultTs(100, 1);
static const Timestamp kDefaultCommitTs(100, 1);
static const repl::OpTime kDefaultOpTime(kDefaultTs, 1);
static const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest(boost::none, "unittests.change_stream");
static const BSONObj kDefaultSpec = BSON("$changeStream" << BSONObj());
static const BSONObj kShowExpandedEventsSpec =
    BSON("$changeStream" << BSON("showExpandedEvents" << true));
static const BSONObj kShowCommitTimestampSpec =
    BSON("$changeStream" << BSON("showCommitTimestamp" << true));


/**
 * This method is required to avoid a static initialization fiasco resulting from calling
 * UUID::gen() in file static scope.
 */
const UUID& testUuid();

LogicalSessionFromClient testLsid();

Document makeResumeToken(Timestamp ts,
                         ImplicitValue uuid,
                         ImplicitValue docKeyOrOpDesc,
                         std::string_view operationType,
                         ResumeTokenData::FromInvalidate fromInvalidate =
                             ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                         size_t txnOpIndex = 0);

Document makeResumeTokenWithEventId(Timestamp ts,
                                    ImplicitValue uuid,
                                    ImplicitValue eventIdentifier,
                                    ResumeTokenData::FromInvalidate fromInvalidate =
                                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                    size_t txnOpIndex = 0);

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj object,
                                boost::optional<UUID> uuid = testUuid(),
                                boost::optional<bool> fromMigrate = boost::none,
                                boost::optional<BSONObj> object2 = boost::none,
                                boost::optional<repl::OpTime> opTime = boost::none,
                                OperationSessionInfo sessionInfo = {},
                                boost::optional<repl::OpTime> prevOpTime = {},
                                boost::optional<repl::OpTime> preImageOpTime = boost::none);
}  // namespace mongo::change_stream_test_helper
