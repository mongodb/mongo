/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update_index_data.h"


namespace mongo::doc_diff {

struct DiffResult {
    Diff diff;
    bool indexesAffected;  // Whether the index data need to be modified if the diff is applied.
};

/**
 * Returns the delta between 'pre' and 'post' by recursively iterating the object. If the size
 * of the computed delta is larger than the 'post' object then the function returns
 * 'boost::none'. The 'paddingForDiff' represents the additional size that needs be added to the
 * size of the diff, while comparing whether the diff is viable. If any paths in 'indexData' are
 * affected by the generated diff, then the 'indexesAffected' field in the output will be set to
 * true, false otherwise.
 */
boost::optional<DiffResult> computeDiff(const BSONObj& pre,
                                        const BSONObj& post,
                                        size_t paddingForDiff,
                                        const UpdateIndexData* indexData);

};  // namespace mongo::doc_diff
