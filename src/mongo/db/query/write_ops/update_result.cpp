// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/write_ops/update_result.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

UpdateResult::UpdateResult(bool existing,
                           bool modifiers,
                           unsigned long long numDocsModified,
                           unsigned long long numMatched,
                           const BSONObj& upsertedObject,
                           bool dotsAndDollarsField)
    : existing(existing),
      modifiers(modifiers),
      numDocsModified(numDocsModified),
      numMatched(numMatched),
      containsDotsAndDollarsField(dotsAndDollarsField) {
    BSONElement id = upsertedObject["_id"];
    if (!existing && numMatched == 0 && !id.eoo()) {
        upsertedId = id.wrap(kUpsertedFieldName);
    }
    LOGV2_DEBUG(20885,
                4,
                "UpdateResult",
                "numMatched"_attr = numMatched,
                "numModified"_attr = numDocsModified,
                "upsertedId"_attr = redact(upsertedId),
                "modifiers"_attr = modifiers,
                "existing"_attr = existing);
}

std::string UpdateResult::toString() const {
    return str::stream() << " upsertedId: " << upsertedId << " modifiers: " << modifiers
                         << " existing: " << existing << " numDocsModified: " << numDocsModified
                         << " numMatched: " << numMatched;
}

}  // namespace mongo
