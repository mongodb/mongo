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

#include "mongo/platform/basic.h"

#include "mongo/db/index/sort_key_generator.h"

#include "mongo/bson/bsonobj_comparator.h"

namespace mongo {

SortKeyGenerator::SortKeyGenerator(const BSONObj& sortSpec, const CollatorInterface* collator)
    : _collator(collator) {
    BSONObjBuilder btreeBob;

    for (auto&& elt : sortSpec) {
        if (elt.isNumber()) {
            btreeBob.append(elt);
            _patternPartTypes.push_back(SortPatternPartType::kFieldPath);
        } else {
            // If this field of the sort pattern is non-numeric, we expect it to be a text-score
            // meta sort.
            invariant(elt.type() == BSONType::Object);
            invariant(elt.embeddedObject().nFields() == 1);
            auto metaElem = elt.embeddedObject().firstElement();
            invariant(metaElem.fieldNameStringData() == "$meta"_sd);
            if (metaElem.valueStringData() == "textScore"_sd) {
                _patternPartTypes.push_back(SortPatternPartType::kMetaTextScore);
            } else {
                invariant(metaElem.valueStringData() == "randVal"_sd);
                _patternPartTypes.push_back(SortPatternPartType::kMetaRandVal);
            }
            _sortHasMeta = true;
        }
    }

    // The fake index key pattern used to generate Btree keys.
    _sortSpecWithoutMeta = btreeBob.obj();

    // If we're just sorting by meta, don't bother with all the key stuff.
    if (_sortSpecWithoutMeta.isEmpty()) {
        return;
    }

    // We'll need to treat arrays as if we were to create an index over them. that is, we may need
    // to unnest the first level and consider each array element to decide the sort order. In order
    // to do this, we make a BtreeKeyGenerator.
    std::vector<const char*> fieldNames;
    std::vector<BSONElement> fixed;
    for (auto&& patternElt : _sortSpecWithoutMeta) {
        fieldNames.push_back(patternElt.fieldName());
        fixed.push_back(BSONElement());
    }

    constexpr bool isSparse = false;
    _indexKeyGen = stdx::make_unique<BtreeKeyGeneratorV1>(fieldNames, fixed, isSparse, _collator);
}

StatusWith<BSONObj> SortKeyGenerator::getSortKey(const BSONObj& obj,
                                                 const Metadata* metadata) const {
    if (_sortHasMeta) {
        invariant(metadata);
    }

    auto indexKey = getIndexKey(obj);
    if (!indexKey.isOK()) {
        return indexKey;
    }

    if (!_sortHasMeta) {
        // We don't have to worry about $meta sort, so the index key becomes the sort key.
        return indexKey;
    }

    BSONObjBuilder mergedKeyBob;

    // Merge metadata into the key.
    BSONObjIterator sortKeyIt(indexKey.getValue());
    for (auto type : _patternPartTypes) {
        switch (type) {
            case SortPatternPartType::kFieldPath: {
                invariant(sortKeyIt.more());
                mergedKeyBob.append(sortKeyIt.next());
                continue;
            }
            case SortPatternPartType::kMetaTextScore: {
                mergedKeyBob.append("", metadata->textScore);
                continue;
            }
            case SortPatternPartType::kMetaRandVal: {
                mergedKeyBob.append("", metadata->randVal);
                continue;
            }
            default: { MONGO_UNREACHABLE; }
        }
    }

    // We should have added a key component for each part of the index key pattern.
    invariant(!sortKeyIt.more());

    return mergedKeyBob.obj();
}

StatusWith<BSONObj> SortKeyGenerator::getIndexKey(const BSONObj& obj) const {
    // Not sorting by anything in the key, just bail out early.
    if (_sortSpecWithoutMeta.isEmpty()) {
        return BSONObj();
    }

    // We will sort 'obj' in the same order an index over '_sortSpecWithoutMeta' would have. This is
    // tricky. Consider the sort pattern {a:1} and the document {a: [1, 10]}. We have potentially
    // two keys we could use to sort on. Here we extract these keys.
    //
    // The keys themselves will incorporate the collation, with strings translated to their
    // corresponding collation keys. Therefore, we use the simple string comparator when comparing
    // the keys themselves.
    const StringData::ComparatorInterface* stringComparator = nullptr;
    BSONObjComparator patternCmp(
        _sortSpecWithoutMeta, BSONObjComparator::FieldNamesMode::kConsider, stringComparator);
    BSONObjSet keys = patternCmp.makeBSONObjSet();

    try {
        // There's no need to compute the prefixes of the indexed fields that cause the index to be
        // multikey when getting the index keys for sorting.
        MultikeyPaths* multikeyPaths = nullptr;
        _indexKeyGen->getKeys(obj, &keys, multikeyPaths);
    } catch (const UserException& e) {
        // Probably a parallel array.
        if (ErrorCodes::CannotIndexParallelArrays == e.getCode()) {
            return Status(ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");
        } else {
            return e.toStatus();
        }
    } catch (...) {
        return Status(ErrorCodes::InternalError, "unknown error during sort key generation");
    }

    // Key generator isn't sparse so we should at least get an all-null key.
    invariant(!keys.empty());

    // The sort key is the first index key, ordered according to the pattern '_sortSpecWithoutMeta'.
    return *keys.begin();
}

}  // namespace mongo
