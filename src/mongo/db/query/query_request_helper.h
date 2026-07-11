// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/tailable_mode.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/serialization_context.h"

#include <memory>
#include <string>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class Status;
template <typename T>
class StatusWith;

/**
 * Parses the find command received from the user and makes the various fields more easily
 * accessible.
 */
namespace query_request_helper {

// Field names for sorting options.
static constexpr auto kNaturalSortField = "$natural";

/**
 * Assert that collectionName is valid.
 */
Status validateGetMoreCollectionName(std::string_view collectionName);

/**
 * Returns a non-OK status if '$_resumeAfter' or '$_startAt' is set to an unexpected value, or the
 * wrong type determined by the collection type.
 */
Status validateResumeInput(OperationContext* opCtx,
                           const mongo::BSONObj& resumeAfter,
                           const mongo::BSONObj& startAt,
                           bool isClusteredCollection);

/**
 * Returns a non-OK status if any property of the QR has a bad value (e.g. a negative skip
 * value) or if there is a bad combination of options (e.g. awaitData is illegal without
 * tailable).
 */
[[MONGO_MOD_PUBLIC]] Status validateFindCommandRequest(const FindCommandRequest& findCommand);

/**
 * Parses a find command object, 'cmdObj'. Caller must indicate whether or not this lite
 * parsed query is an explained query or not via 'isExplain'. Accepts a NSS with which
 * to initialize the FindCommandRequest if there is no UUID in cmdObj.
 *
 * Returns a heap allocated FindCommandRequest on success or an error if 'cmdObj' is not well
 * formed.
 */
[[MONGO_MOD_PUBLIC]] std::unique_ptr<FindCommandRequest> makeFromFindCommand(
    const BSONObj& cmdObj,
    const boost::optional<auth::ValidatedTenancyScope>& vts,
    const boost::optional<TenantId>& tenantId,
    const SerializationContext& sc);

/**
 * Copies an already-parsed FindCommandRequest and applies post-parse normalization and
 * validation (meta projection, skip/limit normalization, option validation).
 */
[[MONGO_MOD_PUBLIC]] std::unique_ptr<FindCommandRequest> makeFromFindCommand(
    const FindCommandRequest& findCommand);

[[MONGO_MOD_PUBLIC]] std::unique_ptr<FindCommandRequest> makeFromFindCommandForTests(
    const BSONObj& cmdObj, boost::optional<NamespaceString> nss = boost::none);

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
[[MONGO_MOD_PUBLIC]] static constexpr auto cmdOptionMaxTimeMS =
    GenericArguments::kMaxTimeMSFieldName;
[[MONGO_MOD_PUBLIC]] static constexpr auto queryOptionMaxTimeMS = "$maxTimeMS";

// Names of the $meta projection values.
static constexpr auto metaGeoNearDistance = "geoNearDistance";
static constexpr auto metaGeoNearPoint = "geoNearPoint";
static constexpr auto metaRecordId = "recordId";
static constexpr auto metaSortKey = "sortKey";
static constexpr auto metaTextScore = "textScore";


static constexpr auto kMaxTimeMSOpOnlyField = "maxTimeMSOpOnly";

void setTailableMode(TailableModeEnum tailableMode, FindCommandRequest* findCommand);

TailableModeEnum getTailableMode(const FindCommandRequest& findCommand);

/**
 * Asserts whether the cursor response adhere to the format defined in IDL.
 */
void validateCursorResponse(const BSONObj& outputAsBson,
                            const boost::optional<auth::ValidatedTenancyScope>& vts,
                            boost::optional<TenantId> tenantId,
                            const SerializationContext& serializationContext);

/**
 * Updates the projection object with a $meta projection for the showRecordId option.
 */
void addShowRecordIdMetaProj(FindCommandRequest* findCommand);

/**
 * Helper that returns true if $natural exists as a key in the passed-in BSONObj, and the value does
 * not equal -1 or 1.
 */
bool hasInvalidNaturalParam(const BSONObj& obj);

[[MONGO_MOD_PUBLIC]] long long getDefaultBatchSize();

}  // namespace query_request_helper
}  // namespace mongo
