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
#include "mongo/db/field_ref.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/dynamic_bitset.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

namespace mongo::doc_diff {

/**
 * A bitset containing one bit for every index in a collection. This is used to indicate which index
 * of a given collection will be affected by a document write operation and a given document diff.
 * Used by the 'determineAfffectedIndexes' method below. The bit positions in an IndexSet should
 * correspond to the position of the index in the list of ready and in progress indexes of a
 * collection as stored in the index catalog (first all '_readyIndexes', then all '_buildingIndexes'
 * of a collection in the collection catalog).
 * Note that an IndexSet always contains 64 bits, as 64 bits is the maximum number of indexes that
 * can be used for a collection. A collection likely has less than 64 indexes, and in this case all
 * excess bits are set to 0.
 */
using IndexSet = DynamicBitset<std::uint64_t, 1>;

/**
 * This class can quickly answer which indexes (if any) of a collection need to be maintained
 * when a document write operation is made on certain fields. When an object of this class is
 * created, the number of (ready and in progress) indexes needs to be passed to the constructor.
 * For each ready and in progress index, a later call to 'addIndex' is expected later, to set
 * the appropriate bits in the IndexSets managed by the object.
 */
class IndexUpdateIdentifier {
public:
    /**
     * Creates the object. The number of indexes here is the number of (ready and in progress)
     * indexes of the collection. For each such index, a later to call to 'addIndex' is expected
     * to fill in the bits in the IndexSets managed by this class.
     */
    explicit IndexUpdateIdentifier(size_t numIndexes);

    /**
     * Adds index 'updateIndexData' to this structure.
     * The 'indexCounter' passed in here is the position of the index in the list of (ready and
     * in progress) indexes of the collection.
     */
    void addIndex(size_t indexCounter, const UpdateIndexData& updateIndexData);

    /**
     * Returns an IndexSet with a bit set for each index that may be affected by the update
     * operation. Note that the bits positions correspond to the position of ready and in
     * progress indexes in the index catalog ('_readyIndexes' and '_buildingIndexes'). Returns
     * an empty IndexSet if the diff is empty.
     */
    [[nodiscard]] IndexSet determineAffectedIndexes(const Diff& diff) const;

private:
    /**
     * Number of indexes contained in the IndexSet.
     * This is const because we build a new instance of IndexUpdateIdentifier whenever the
     * list of indexes for a collection changes.
     */
    const size_t _numIndexes;

    /**
     * Updates the IndexSet 'indexesToUpdate' for each index that is affected by a write to the
     * given field path 'path'. A bit will be set in the IndexSet for each index that may be
     * affected.
     */
    void determineAffectedIndexes(const FieldRef& path, IndexSet& indexesToUpdate) const;

    /**
     * Same as 'determineAffectedIndexes(FieldRef&, IndexSet&)', but iterating over a sub-array
     * field. The 'fieldRef' parameter here contains the name of the field on the parent level.
     */
    void determineAffectedIndexes(ArrayDiffReader* reader,
                                  FieldRef& fieldRef,
                                  IndexSet& indexesToUpdate) const;

    /**
     * Same as 'determineAffectedIndexes(FieldRef&, IndexSet&)', but iterating over the
     * key/values of a sub-document. The 'fieldRef' parameter here contains the name of the
     * field on the parent level.
     */
    void determineAffectedIndexes(DocumentDiffReader* reader,
                                  FieldRef& fieldRef,
                                  IndexSet& indexesToUpdate) const;

    /**
     * IndexSet containing a set bit for each index that is a wildcard index ({"$**": 0|1}) or a
     * wildcard text index ({"$**": "text"}). Such indexes assume responsibility for all fields.
     * The IndexSet has a size equal to '_numIndexes'.
     */
    IndexSet _allPathsIndexed;

    /**
     * How many canonicalized paths and indexed fields we are going to store inline as an
     * optimization for the case that few indexes exist.
     */
    static constexpr size_t kNumFieldRefsStoredInline = 8;

    /**
     * A mapping from a canonicalized path of an indexed field to a set of indexes that include
     * that field. All IndexSets here have a size equal to '_numIndexes'.
     */
    absl::InlinedVector<std::pair<FieldRef, doc_diff::IndexSet>, kNumFieldRefsStoredInline>
        _canonicalPathsToIndexSets;

    /**
     * A mapping from a path component of an index to a set of indexes that include that path
     * component. Path components contain the name(s) of the "language" field(s) in text
     * indexes, if any. All IndexSets here have a size equal to '_numIndexes'.
     */
    absl::flat_hash_map<std::string, doc_diff::IndexSet> _pathComponentsToIndexSets;
};

/**
 * Returns the oplog v2 diff between the given 'pre' and 'post' images. The diff has the
 * following format:
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
 * Same as 'computeOplogDiff(...)', but also returns the diff if it is larger than the 'post'
 * image is. Useful in testing, but not in production.
 */
Diff computeOplogDiff_forTest(const BSONObj& pre, const BSONObj& post);

/**
 * Returns the inline diff between the given 'pre' and 'post' images. The diff has the same
 * schema as the document that the images correspond to. The value of each field is set to
 * either 'i', 'u' or 'd' to indicate that the field was inserted, updated and deleted,
 * respectively. The fields that did not change do not show up in the diff. For example:
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

};  // namespace mongo::doc_diff
