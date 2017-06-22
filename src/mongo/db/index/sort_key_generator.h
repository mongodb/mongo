/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"

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
     * Constructs a sort key generator which will generate keys for sort pattern 'sortSpec'. The
     * keys will incorporate the collation given by 'collator', and thus when actually compared to
     * one another should use the simple collation.
     */
    SortKeyGenerator(const BSONObj& sortSpec, const CollatorInterface* collator);

    /**
     * Returns the key which should be used to sort 'obj', or a non-OK status if no key could be
     * generated.
     *
     * The caller must supply the appropriate 'metadata' in the case that the sort pattern includes
     * a $meta sort (i.e. if sortHasMeta() is true). These values are filled in at the corresponding
     * positions in the sort key.
     */
    StatusWith<BSONObj> getSortKey(const BSONObj& obj, const Metadata*) const;

    /**
     * Returns true if the sort pattern for this sort key generator includes a $meta sort.
     */
    bool sortHasMeta() const {
        return _sortHasMeta;
    }

private:
    // Describes whether a component of the sort pattern is a field path (e.g. sort by "a.b"), or
    // else describes the type of $meta sort.
    enum class SortPatternPartType {
        kFieldPath,
        kMetaTextScore,
        kMetaRandVal,
    };

    StatusWith<BSONObj> getIndexKey(const BSONObj& obj) const;

    const CollatorInterface* _collator = nullptr;

    // The sort pattern with any $meta sort components stripped out, since the underlying index key
    // generator does not understand $meta sort.
    BSONObj _sortSpecWithoutMeta;

    // For each element of the raw sort spec, describes whether the element is sorting by a field
    // path or by a particular meta-sort.
    std::vector<SortPatternPartType> _patternPartTypes;

    // If we're not sorting with a $meta value we can short-cut some work.
    bool _sortHasMeta = false;

    std::unique_ptr<BtreeKeyGenerator> _indexKeyGen;
};

}  // namespace mongo
