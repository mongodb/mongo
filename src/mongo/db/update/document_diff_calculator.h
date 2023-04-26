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

#include <boost/dynamic_bitset.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update_index_data.h"

namespace mongo::doc_diff {

/**
 * Returns the oplog v2 diff between the given 'pre' and 'post' images. The diff has the following
 * format:
 * {
 *    i: {<fieldName>: <value>, ...},                       // optional insert section.
 *    u: {<fieldName>: <newValue>, ...},                    // optional update section.
 *    d: {<fieldName>: false, ...},                         // optional delete section.
 *    s<arrFieldName>: {a: true, l: <integer>, ...},        // array diff.
 *    s<objFieldName>: {i: {...}, u: {...}, d: {...}, ...}, // object diff.
 *    ...
 * }
 * If the size of the computed diff is larger than the 'post' image then the function returns
 * 'boost::none'. The 'paddingForDiff' represents the additional size that needs be added to the
 * size of the diff, while comparing whether the diff is viable.
 */
boost::optional<Diff> computeOplogDiff(const BSONObj& pre,
                                       const BSONObj& post,
                                       size_t paddingForDiff);

/**
 * Returns the inline diff between the given 'pre' and 'post' images. The diff has the same schema
 * as the document that the images correspond to. The value of each field is set to either 'i',
 * 'u' or 'd' to indicate that the field was inserted, updated and deleted, respectively. The
 * fields that did not change do not show up in the diff. For example:
 * {
 *    <fieldName>: 'i'|'u'|'d',
 *    <arrFieldName>: 'i'|'u'|'d',
 *    <objFieldName>: {
 *       <fieldName>: 'i'|'u'|'d',
 *       ...,
 *    },
 *    ...
 * }
 * Returns 'boost::none' if the diff exceeds the BSON size limit.
 */
boost::optional<BSONObj> computeInlineDiff(const BSONObj& pre, const BSONObj& post);

using BitVector = boost::dynamic_bitset<size_t>;
/**
 * Returns a bitset of the same size of the indexData argument, where each bit indicates
 * whether one of the modifications described in the diff document affects one of the
 * indexed paths described in the matching object.
 */
BitVector anyIndexesMightBeAffected(const Diff& diff,
                                    const std::vector<const UpdateIndexData*>& indexData);

};  // namespace mongo::doc_diff
