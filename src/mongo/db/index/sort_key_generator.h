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
     * Metadata about a document which is needed to produce keys for $meta sort. The client of the
     * SortKeyGenerator must provide this metadata in order to correctly obtain sort keys for sort
     * patterns with $meta.
     */
    struct Metadata {
        double textScore = 0.0;
        double randVal = 0.0;
    };

    /**
     * Constructs a sort key generator which will generate keys for sort pattern 'sortPattern'. The
     * keys will incorporate the collation given by 'collator', and thus when actually compared to
     * one another should use the simple collation.
     */
    SortKeyGenerator(SortPattern sortPattern, const CollatorInterface* collator);

    /**
     * Returns the key which should be used to sort the WorkingSetMember, or a non-OK status if no
     * key could be generated. The WorkingSetMember may represent either an index key, or a document
     * (owned or unowned) that has been fetched from the collection.
     *
     * If the sort pattern contains a $meta sort (e.g. sort by "textScore" or "randVal"), then the
     * necessary metadata is obtained from the WorkingSetMember.
     */
    StatusWith<BSONObj> getSortKey(const WorkingSetMember&) const;

    /**
     * Returns the key which should be used to sort 'obj', or a non-OK status if no key could be
     * generated.
     *
     * The caller must supply the appropriate 'metadata' in the case that the sort pattern includes
     * a $meta sort (i.e. if sortHasMeta() is true). These values are filled in at the corresponding
     * positions in the sort key.
     */
    StatusWith<BSONObj> getSortKeyFromDocument(const BSONObj& obj, const Metadata*) const;

private:
    // Extracts the sort key from a WorkingSetMember which represents an index key. It is illegal to
    // call this if the working set member is not in RID_AND_IDX state. It is also illegal to call
    // this if the sort pattern has any $meta components.
    StatusWith<BSONObj> getSortKeyFromIndexKey(const WorkingSetMember& member) const;

    // Extracts the sort key from 'obj', using '_sortSpecWithoutMeta' and thus ignoring any $meta
    // sort components of the sort pattern. The caller is responsible for augmenting this key with
    // the appropriate metadata if '_sortHasMeta' is true.
    StatusWith<BSONObj> getSortKeyFromDocumentWithoutMetadata(const BSONObj& obj) const;

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
