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

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"

/**
 * Formats Aggregation Pipeline results as legacy mapReduce output.
 */
namespace mongo::map_reduce_output_format {

/**
 * Appends fields to 'resultBuilder' as if 'documents' was a response from the mapReduce command
 * with inline output. 'verbose' causes extra fields to be appended to the response in accordance
 * with the verbose option on the mapReduce command. 'inMongos' indicates that we are using the
 * format that was historically sent from mongos. If it isn't set, we will use the mongod format.
 */
void appendInlineResponse(BSONArray&& documents,
                          bool verbose,
                          bool inMongos,
                          BSONObjBuilder* resultBuilder);

/**
 * Appends fields to 'resultBuilder' to formulate a response that would be given if the mapReduce
 * command had written output to the collection named by 'outCollNss'. 'verbose' causes extra fields
 * to be appended to the response in accordance with the verbose option on the mapReduce command.
 * 'inMongos' indicates that we are using the format that was historically sent from mongos. If it
 * isn't set, we will use the mongod format.
 */
void appendOutResponse(NamespaceString outCollNss,
                       bool verbose,
                       bool inMongos,
                       BSONObjBuilder* resultBuilder);

}  // namespace mongo::map_reduce_output_format
