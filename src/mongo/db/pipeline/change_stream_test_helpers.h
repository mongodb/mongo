/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
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
#include "mongo/util/uuid.h"

#include <cstddef>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
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
                         StringData operationType,
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
