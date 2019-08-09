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

#include "mongo/db/query/map_reduce_output_format.h"

namespace mongo::map_reduce_output_format {

namespace {

void appendMetadataFields(bool verbose, bool inMongos, BSONObjBuilder* resultBuilder) {
    // TODO: SERVER-42644 Build stats (encapsulate in functions!).
    if (inMongos)
        resultBuilder->append("counts",
                              BSON("input" << 0 << "emit" << 0 << "reduce" << 0 << "output" << 0));

    resultBuilder->append("timeMillis", 0);

    if (verbose) {
        auto&& timingField = inMongos
            ? BSON("shardProcessing" << 0 << "postProcessing" << 0)
            : BSON("mapTime" << 0 << "emitLoop" << 0 << "reduceTime" << 0 << "total" << 0);
        resultBuilder->append("timing", timingField);
    }

    if (inMongos) {
        resultBuilder->append(
            "shardCounts",
            BSON("shard-conn-string"
                 << BSON("input" << 0 << "emit" << 0 << "reduce" << 0 << "output" << 0)));
        resultBuilder->append("postProcessCounts",
                              BSON("merging-shard-conn-string"
                                   << BSON("input" << 0 << "reduce" << 0 << "output" << 0)));
    } else {
        resultBuilder->append("counts",
                              BSON("input" << 0 << "emit" << 0 << "reduce" << 0 << "output" << 0));
    }

    resultBuilder->append("ok", 1);
}

}  // namespace

void appendInlineResponse(BSONArray&& documents,
                          bool verbose,
                          bool inMongos,
                          BSONObjBuilder* resultBuilder) {
    resultBuilder->appendArray("results", documents);
    appendMetadataFields(verbose, inMongos, resultBuilder);
}

void appendOutResponse(NamespaceString outCollNss,
                       bool verbose,
                       bool inMongos,
                       BSONObjBuilder* resultBuilder) {
    if (outCollNss.db().empty())
        resultBuilder->append("result", outCollNss.coll());
    else
        resultBuilder->append("result",
                              BSON("db" << outCollNss.db() << "collection" << outCollNss.coll()));
    appendMetadataFields(verbose, inMongos, resultBuilder);
}
}  // namespace mongo::map_reduce_output_format
