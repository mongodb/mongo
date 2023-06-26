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


#include "mongo/db/ops/update_result.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
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
                "UpdateResult -- upserted: {upserted} modifiers: {modifiers} existing: {existing} "
                "numDocsModified: {numModified} numMatched: {numMatched}",
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
