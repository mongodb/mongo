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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/sort_pattern.h"

namespace mongo {

class SortKeyGenerator {
public:
    /**
     * Constructs a sort key generator which will generate keys for sort pattern 'sortPattern'. The
     * keys will incorporate the collation given by 'collator', and thus when actually compared to
     * one another should use the simple collation.
     */
    SortKeyGenerator(SortPattern sortPattern, const CollatorInterface* collator);

    /**
     * Returns the key which should be used to sort the WorkingSetMember or throws if no key could
     * be generated. The WorkingSetMember may represent either an index key or a document (owned or
     * unowned) that has been fetched from the collection.
     *
     * If the sort pattern contains a $meta sort (e.g. sort by "textScore" or "randVal"), then the
     * necessary metadata is obtained from the WorkingSetMember.
     */
    Value computeSortKey(const WorkingSetMember&) const;

    /**
     * Returns the sort key for the input 'doc' as a Value or throws if no key could be generated.
     * When the sort pattern has multiple components, the resulting sort key is an Array-typed Value
     * with one element for each component. For sort patterns with just one component, the sort key
     * is a Value that represents the single element to sort on (which may or may not itself be an
     * array).
     *
     * The sort key is computed based on the sort pattern, the contents of the document, and if
     * required by $meta sort specifiers, metadata in the Document.
     */
    Value computeSortKeyFromDocument(const Document& doc) const {
        return computeSortKeyFromDocument(doc, doc.metadata());
    }

    bool isSingleElementKey() const {
        return _sortPattern.isSingleElementKey();
    }

private:
    // Returns the sort key for the input 'doc' as a Value.
    //
    // Note that this function will ignore any metadata (e.g., textScore, randVal), in 'doc' but
    // will instead read from the 'metadata' variable. When the metadata is contained in the 'doc'
    // input, callers can use the public overload of this function.
    Value computeSortKeyFromDocument(const Document& doc,
                                     const DocumentMetadataFields& metadata) const;

    // Returns the key which should be used to sort 'obj' or throws an exception if no key could be
    // generated.
    //
    // The caller must supply the appropriate 'metadata' in the case that the sort pattern includes
    // a $meta sort (i.e. if sortHasMeta() is true). These values are filled in at the corresponding
    // positions in the sort key.
    BSONObj computeSortKeyFromDocument(const BSONObj& obj,
                                       const DocumentMetadataFields& metadata) const;

    // Extracts the sort key from a WorkingSetMember which represents an index key. It is illegal to
    // call this if the working set member is not in RID_AND_IDX state. It is also illegal to call
    // this if the sort pattern has any $meta components.
    Value computeSortKeyFromIndexKey(const WorkingSetMember& member) const;

    // Extracts the sort key from 'obj', using '_sortSpecWithoutMeta' and thus ignoring any $meta
    // sort components of the sort pattern. The caller is responsible for augmenting this key with
    // the appropriate metadata if '_sortHasMeta' is true.
    StatusWith<BSONObj> computeSortKeyFromDocumentWithoutMetadata(const BSONObj& obj) const;

    // Returns the sort key for 'doc' based on the SortPattern, or ErrorCodes::InternalError if an
    // array is encountered during sort key generation.
    //
    // Note that this function will ignore any metadata (e.g., textScore, randVal), in 'doc' but
    // will instead read from the 'metadata' variable.
    StatusWith<Value> extractKeyFast(const Document& doc,
                                     const DocumentMetadataFields& metadata) const;

    // Extracts the sort key component described by 'keyPart' from 'doc' and returns it. Returns
    // ErrorCodes::InternalError if the path for 'keyPart' contains an array in 'doc'.
    //
    // Note that this function will ignore any metadata (e.g., textScore, randVal), in 'doc' but
    // will instead read from the 'metadata' variable.
    StatusWith<Value> extractKeyPart(const Document& doc,
                                     const DocumentMetadataFields& metadata,
                                     const SortPattern::SortPatternPart& keyPart) const;

    // Returns the sort key for 'doc' based on the SortPattern. Note this is in the BSONObj format -
    // with empty field names.
    //
    // Note that this function will ignore any metadata (e.g., textScore, randVal), in 'doc' but
    // will instead read from the 'metadata' variable.
    BSONObj extractKeyWithArray(const Document& doc, const DocumentMetadataFields& metadata) const;

    // Returns the comparison key used to sort 'val' with collation. Note that these comparison keys
    // should always be sorted with the simple (i.e. binary) collation.
    Value getCollationComparisonKey(const Value& val) const;

    const CollatorInterface* _collator = nullptr;

    SortPattern _sortPattern;

    // The sort pattern with any $meta sort components stripped out, since the underlying index key
    // generator does not understand $meta sort.
    BSONObj _sortSpecWithoutMeta;

    // If we're not sorting with a $meta value we can short-cut some work.
    bool _sortHasMeta = false;

    std::unique_ptr<BtreeKeyGenerator> _indexKeyGen;
};

}  // namespace mongo
