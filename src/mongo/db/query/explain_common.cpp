/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/explain_common.h"

#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/version.h"

namespace mongo::explain_common {

void generateServerInfo(BSONObjBuilder* out) {
    BSONObjBuilder serverBob(out->subobjStart("serverInfo"));
    out->append("host", getHostNameCached());
    out->appendNumber("port", serverGlobalParams.port);
    auto&& vii = VersionInfoInterface::instance();
    out->append("version", vii.version());
    out->append("gitVersion", vii.gitVersion());
    serverBob.doneFast();
}

void generateServerParameters(BSONObjBuilder* out) {
    BSONObjBuilder serverBob(out->subobjStart("serverParameters"));
    out->appendNumber("internalQueryFacetBufferSizeBytes",
                      internalQueryFacetBufferSizeBytes.load());
    out->appendNumber("internalQueryFacetMaxOutputDocSizeBytes",
                      internalQueryFacetMaxOutputDocSizeBytes.load());
    out->appendNumber("internalLookupStageIntermediateDocumentMaxSizeBytes",
                      internalLookupStageIntermediateDocumentMaxSizeBytes.load());
    out->appendNumber("internalDocumentSourceGroupMaxMemoryBytes",
                      internalDocumentSourceGroupMaxMemoryBytes.load());
    out->appendNumber("internalQueryMaxBlockingSortMemoryUsageBytes",
                      internalQueryMaxBlockingSortMemoryUsageBytes.load());
    out->appendNumber("internalQueryProhibitBlockingMergeOnMongoS",
                      internalQueryProhibitBlockingMergeOnMongoS.load());
    out->appendNumber("internalQueryMaxAddToSetBytes", internalQueryMaxAddToSetBytes.load());
    out->appendNumber("internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                      internalDocumentSourceSetWindowFieldsMaxMemoryBytes.load());
}

bool appendIfRoom(const BSONObj& toAppend, StringData fieldName, BSONObjBuilder* out) {
    if ((out->len() + toAppend.objsize()) < BSONObjMaxUserSize) {
        out->append(fieldName, toAppend);
        return true;
    }

    // The reserved buffer size for the warning message if 'out' exceeds the max BSON user size.
    const int warningMsgSize = fieldName.size() + 60;

    // Unless 'out' has already exceeded the max BSON user size, add a warning indicating
    // that data has been truncated.
    if (out->len() < BSONObjMaxUserSize - warningMsgSize) {
        out->append("warning",
                    str::stream() << "'" << fieldName << "'"
                                  << " has been omitted due to BSON size limit");
    }

    return false;
}

}  // namespace mongo::explain_common
