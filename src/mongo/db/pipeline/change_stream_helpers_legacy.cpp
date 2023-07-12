/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/pipeline/change_stream_helpers_legacy.h"

#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace change_stream_legacy {

// TODO SERVER-66138: This function can be removed after we branch for 7.0.
void populateInternalOperationFilter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     BSONArrayBuilder* orBuilder) {
    std::vector<StringData> opTypes = {"reshardBegin"_sd, "reshardDoneCatchUp"_sd};

    // Noop change events that are only applicable when merging results on mongoS:
    //   - migrateChunkToNewShard: A chunk migrated to a shard that didn't have any chunks.
    if (expCtx->inMongos || expCtx->needsMerge) {
        opTypes.push_back("migrateChunkToNewShard"_sd);
    }

    for (const auto& eventName : opTypes) {
        // Legacy oplog messages used the "o2.type" field to indicate the message type.
        orBuilder->append(BSON("o2.type" << eventName));
    }
}

// TODO SERVER-66138: This function can be removed after we branch for 7.0.
Document convertFromLegacyOplogFormat(const Document& o2Entry, const NamespaceString& nss) {
    auto type = o2Entry["type"];
    if (type.missing()) {
        return o2Entry;
    }

    MutableDocument doc(o2Entry);
    doc.remove("type");

    // This field would be the first field in the new format, but the current change stream code
    // does not depend on the field order.
    doc.addField(type.getString(), Value(NamespaceStringUtil::serialize(nss)));
    return doc.freeze();
}
}  // namespace change_stream_legacy
}  // namespace mongo
