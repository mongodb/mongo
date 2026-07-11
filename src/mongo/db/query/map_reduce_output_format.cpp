// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/map_reduce_output_format.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"

#include <boost/optional/optional.hpp>

namespace mongo::map_reduce_output_format {

void appendInlineResponse(BSONArray&& documents, BSONObjBuilder* resultBuilder) {
    resultBuilder->appendArray("results", documents);
}

void appendOutResponse(boost::optional<std::string> outDb,
                       std::string outColl,
                       BSONObjBuilder* resultBuilder) {
    if (outDb) {
        resultBuilder->append("result", BSON("db" << *outDb << "collection" << outColl));
    } else {
        resultBuilder->append("result", outColl);
    }
}

void appendExplainResponse(BSONObjBuilder& resultBuilder, BSONObj& aggResults) {
    for (const auto& elem : aggResults) {
        resultBuilder << elem.fieldNameStringData() << elem;
    }
}

}  // namespace mongo::map_reduce_output_format
