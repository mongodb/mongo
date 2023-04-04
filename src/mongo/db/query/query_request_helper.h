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

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/tailable_mode.h"

namespace mongo {

class Status;
template <typename T>
class StatusWith;

/**
 * Parses the find command received from the user and makes the various fields more easily
 * accessible.
 */
namespace query_request_helper {

static constexpr auto kMaxTimeMSOpOnlyField = "maxTimeMSOpOnly";

// Field names for sorting options.
static constexpr auto kNaturalSortField = "$natural";

/**
 * Assert that collectionName is valid.
 */
Status validateGetMoreCollectionName(StringData collectionName);

/**
 * Returns a non-OK status if any property of the QR has a bad value (e.g. a negative skip
 * value) or if there is a bad combination of options (e.g. awaitData is illegal without
 * tailable).
 */
Status validateFindCommandRequest(const FindCommandRequest& findCommand);

/**
 * Parses a find command object, 'cmdObj'. Caller must indicate whether or not this lite
 * parsed query is an explained query or not via 'isExplain'. Accepts a NSS with which
 * to initialize the FindCommandRequest if there is no UUID in cmdObj.
 *
 * Returns a heap allocated FindCommandRequest on success or an error if 'cmdObj' is not well
 * formed.
 */
std::unique_ptr<FindCommandRequest> makeFromFindCommand(const BSONObj& cmdObj,
                                                        boost::optional<NamespaceString> nss,
                                                        bool apiStrict);

std::unique_ptr<FindCommandRequest> makeFromFindCommandForTests(
    const BSONObj& cmdObj,
    boost::optional<NamespaceString> nss = boost::none,
    bool apiStrict = false);

/**
 * If _uuid exists for this FindCommandRequest, update the value of _nss.
 */
void refreshNSS(const NamespaceString& nss, FindCommandRequest* findCommand);

/**
 * Converts this FindCommandRequest into an aggregation using $match. If this FindCommandRequest has
 * options that cannot be satisfied by aggregation, a non-OK status is returned and 'cmdBuilder' is
 * not modified.
 */
StatusWith<BSONObj> asAggregationCommand(const FindCommandRequest& findCommand);

/**
 * Helper function to identify text search sort key
 * Example: {a: {$meta: "textScore"}}
 */
bool isTextScoreMeta(BSONElement elt);

// Historically for the OP_QUERY wire protocol message, read preference was sent to the server in a
// "wrapped" form: {$query: <cmd payload>, $readPreference: ...}. Internally, this was converted to
// the so-called "unwrapped" format for convenience:
//
//   {<cmd payload>, $queryOptions: {$readPreference: ...}}
//
// TODO SERVER-29091: This is a holdover from when OP_QUERY was supported by the server and should
// be deleted.
static constexpr auto kUnwrappedReadPrefField = "$queryOptions";

// Names of the maxTimeMS command and query option.
// Char arrays because they are used in static initialization.
static constexpr auto cmdOptionMaxTimeMS = "maxTimeMS";
static constexpr auto queryOptionMaxTimeMS = "$maxTimeMS";

// Names of the $meta projection values.
static constexpr auto metaGeoNearDistance = "geoNearDistance";
static constexpr auto metaGeoNearPoint = "geoNearPoint";
static constexpr auto metaRecordId = "recordId";
static constexpr auto metaSortKey = "sortKey";
static constexpr auto metaTextScore = "textScore";

// A constant by which 'maxTimeMSOpOnly' values are allowed to exceed the max allowed value for
// 'maxTimeMS'.  This is because mongod and mongos server processes add a small amount to the
// 'maxTimeMS' value they are given before passing it on as 'maxTimeMSOpOnly', to allow for
// clock precision.
static constexpr auto kMaxTimeMSOpOnlyMaxPadding = 100LL;

static constexpr auto kDefaultBatchSize = 101ll;

void setTailableMode(TailableModeEnum tailableMode, FindCommandRequest* findCommand);

TailableModeEnum getTailableMode(const FindCommandRequest& findCommand);

/**
 * Asserts whether the cursor response adhere to the format defined in IDL.
 */
void validateCursorResponse(const BSONObj& outputAsBson, boost::optional<TenantId> tenantId);

/**
 * Updates the projection object with a $meta projection for the showRecordId option.
 */
void addShowRecordIdMetaProj(FindCommandRequest* findCommand);

/**
 * Helper that returns true if $natural exists as a key in the passed-in BSONObj, and the value does
 * not equal -1 or 1.
 */
bool hasInvalidNaturalParam(const BSONObj& obj);

}  // namespace query_request_helper
}  // namespace mongo
