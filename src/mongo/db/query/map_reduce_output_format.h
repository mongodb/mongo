// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/optional/optional.hpp>

/**
 * A collection of functions responsible for building a mapReduce command response from a result
 * set.
 */
namespace mongo::map_reduce_output_format {

/**
 * Appends an inline mapReduce command response to 'resultBuilder'.
 */
void appendInlineResponse(BSONArray&& documents, BSONObjBuilder* resultBuilder);

/**
 * Appends an output-to-collection mapReduce command response to 'resultBuilder'.
 */
void appendOutResponse(boost::optional<std::string> outDb,
                       std::string outColl,
                       BSONObjBuilder* resultBuilder);

/**
 * Appends a mapReduce explain command response to 'resultBuilder'.
 */
void appendExplainResponse(BSONObjBuilder& resultBuilder, BSONObj& aggResults);

}  // namespace mongo::map_reduce_output_format
