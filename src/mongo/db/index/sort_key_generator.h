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
#include "mongo/db/exec/field_name_bloom_filter.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/assert_util.h"

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
     * Computes a KeyString that can be used as the sort key for this object.
     */
    KeyString::Value computeSortKeyString(const BSONObj& bson);

    /**
     * Determines all of the portions of the sort key for the given document and populates the
     * output vector with their positions. The results in the output vector are valid until the
     * next call.
     */
    void generateSortKeyComponentVector(const BSONObj& obj, std::vector<BSONElement>* out);

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

    const SortPattern& getSortPattern() const {
        return _sortPattern;
    }

    void setCollator(const CollatorInterface* c) {
        _collator = c;
    }

    size_t getApproximateSize() const {
        return sizeof(*this) + (_indexKeyGen ? _indexKeyGen->getApproximateSize() : 0) +
            _sortKeyTreeRoot.getApproximateSize() + _sortSpecWithoutMeta.objsize() +
            _localObjStorage.objsize();
    }

private:
    /* Tree representation of the sort key pattern. E.g. {a.b:1, a.x: 1}
     *      a
     *    /  \
     *   b    x
     * This is used for the fast path where the sort key is generated in one pass over the bson.
     * This is only used when the sort pattern does not include a $meta.
     */
    struct SortKeyTreeNode {
        std::string name;
        const SortPattern::SortPatternPart* part = nullptr;  // Pointer into the SortPattern.
        std::vector<std::unique_ptr<SortKeyTreeNode>> children;
        size_t partIdx = 0;

        // Tracks field names of the children. We use this when scanning the bson to quickly skip
        // over fields irrelevant to the sort pattern.
        FieldNameBloomFilter<> bloomFilter;

        // Adds a new component of the sort pattern to the tree.
        void addSortPatternPart(const SortPattern::SortPatternPart* part,
                                const size_t pathIdx,
                                const size_t partIdx) {
            if (pathIdx == part->fieldPath->getPathLength()) {
                tassert(7103700, "Invalid sort tree", !this->part);
                this->part = part;
                this->partIdx = partIdx;
                return;
            }

            for (auto& c : children) {
                // Check if we already have a child with the same prefix.
                if (c->name == part->fieldPath->getFieldName(pathIdx)) {
                    c->addSortPatternPart(part, pathIdx + 1, partIdx);
                    return;
                }
            }

            children.push_back(std::make_unique<SortKeyTreeNode>());
            children.back()->name = part->fieldPath->getFieldName(pathIdx).toString();
            children.back()->addSortPatternPart(part, pathIdx + 1, partIdx);
            bloomFilter.insert(children.back()->name.c_str(), children.back()->name.size());
        }

        size_t getApproximateSize() const {
            size_t size = sizeof(*this) + name.size();
            for (auto& c : children) {
                size += c->getApproximateSize();
            }
            return size;
        }
    };

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

    // Returns the sort key for 'doc' based on the SortPattern, or boost::none if an array is
    // encountered during sort key generation.
    //
    // Note that this function will ignore any metadata (e.g., textScore, randVal), in 'doc' but
    // will instead read from the 'metadata' variable.
    boost::optional<Value> extractKeyFast(const Document& doc,
                                          const DocumentMetadataFields& metadata) const;

    // Extracts the sort key component described by 'keyPart' from 'doc' and returns it. Returns
    // boost::none if the path for 'keyPart' contains an array in 'doc'.
    //
    // Note that this function will ignore any metadata (e.g., textScore, randVal), in 'doc' but
    // will instead read from the 'metadata' variable.
    boost::optional<Value> extractKeyPart(const Document& doc,
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

    // Fast path for reading a BSON obj and computing the sort key. Returns true on success, false
    // when an array is encountered and the slow path needs to be used instead.
    bool fastFillOutSortKeyParts(const BSONObj& bson, std::vector<BSONElement>* out) const;
    bool fastFillOutSortKeyPartsHelper(const BSONObj& bson,
                                       const SortKeyGenerator::SortKeyTreeNode& tree,
                                       std::vector<BSONElement>* out) const;


    const CollatorInterface* _collator = nullptr;

    SortPattern _sortPattern;

    // The sort pattern with any $meta sort components stripped out, since the underlying index key
    // generator does not understand $meta sort.
    BSONObj _sortSpecWithoutMeta;
    Ordering _ordering;

    // If we're not sorting with a $meta value we can short-cut some work.
    bool _sortHasMeta = false;

    std::unique_ptr<BtreeKeyGenerator> _indexKeyGen;

    // Used for fastFillOutSortKeyParts()/extractSortKeyParts().
    SortKeyTreeNode _sortKeyTreeRoot;
    BSONObj _localObjStorage;

    // Used when generating KeyStrings.
    std::vector<BSONElement> _localEltStorage;
};

}  // namespace mongo
