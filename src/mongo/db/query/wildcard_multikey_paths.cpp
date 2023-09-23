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

#include <cstddef>
#include <iterator>
#include <memory>
#include <utility>

#include <absl/container/node_hash_set.h>
#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_metadata_access_stats.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/interval.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

/**
 * A wildcard index contains an unbounded set of multikey paths, therefore, it was decided to store
 * multikey path as a metadata key to a wildcard index using the following special format:
 * {["": MinKey, ] "": 1, "": "multikey.path.value" [, "": MinKey] }.
 * Where MinKey values are corresponding to regular fields of the Wildcard Index, which can never be
 * multikeys, and the actual multikey path value is prefixed by the integer value 1.
 * Some examples of Wildcard key patterns and their corresponding metadata keys
 * - {"$**": 1} --> {"": 1, "some.path": 1}
 * - {a: 1, "$**": 1} --> {"": MinKey, "": 1, "": "some.path"}
 * - {a: 1, "$**": 1, b: 1} --> {"": MinKey, "": 1, "": "some.path", "": MinKey}
 * - {a: 1, c: 1, "$**": 1, b: 1} --> {"": MinKey, "": MinKey, "": 1, "": "some.path", "": MinKey}
 * where "some.path" represents an actual multikey path in some documents.
 * The prefix of a number of MinKey values followed by the number "1" allows to differentiate
 * multikey metadata keys from user-data keys, because user-data keys always have a string value on
 * the position of the value "1" in a multikey metadata key.
 */
namespace mongo {

/**
 * Extracts the multikey path from a metadata key stored within a wildcard index.
 */
static FieldRef extractMultikeyPathFromIndexKey(const IndexKeyEntry& entry) {
    tassert(7354600,
            "A disk location of a Wildcard Index's metadata key must be a reserved value",
            record_id_helpers::isReserved(entry.loc));
    tassert(7354601,
            "A disk location of a Wildcard Index's metadata key must a reserved value of type "
            "string or int",
            !entry.loc.isLong() ||
                entry.loc ==
                    record_id_helpers::reservedIdFor(
                        record_id_helpers::ReservationId::kWildcardMultikeyMetadataId,
                        KeyFormat::Long));
    tassert(7354602,
            "A disk location of a Wildcard Index's metadata key must a reserved value of type "
            "string or int",
            !entry.loc.isStr() ||
                entry.loc ==
                    record_id_helpers::reservedIdFor(
                        record_id_helpers::ReservationId::kWildcardMultikeyMetadataId,
                        KeyFormat::String));

    // Validate that the first piece of the key is the integer 1.
    BSONObjIterator iter(entry.key);
    while (iter.more()) {
        const auto elem = iter.next();
        if (elem.type() != BSONType::MinKey) {
            tassert(7354603,
                    "An int value must follow MinKey values in a metadata key of a wildcard "
                    "index.",
                    elem.isNumber());
            tassert(7354604,
                    "The int value '1' must follow MinKey values in a metadata key of a wildcard "
                    "index.",
                    elem.numberInt() == 1);
            tassert(7354605,
                    "A string value must follow an int value in a metadata key of a wildcard index",
                    iter.more());
            const auto nextElem = iter.next();
            tassert(7354606,
                    "A string value must follow an int value in a metadata key of a wildcard index",
                    nextElem.type() == BSONType::String);
            return FieldRef(nextElem.valueStringData());
        }
    }

    tasserted(7354607,
              str::stream() << "Unexpected format of a metadata key of a wildcard index: "
                            << entry.key);
}

/**
 * Returns IndexBoundsChecker's key pattern for the given Wildcard Index's key pattern.
 */
static BSONObj buildIndexBoundsKeyPattern(const BSONObj& wiKeyPattern) {
    static constexpr StringData emptyFieldName = ""_sd;

    BSONObjBuilder builder{};

    for (const auto& field : wiKeyPattern) {
        if (WildcardNames::isWildcardFieldName(field.fieldNameStringData())) {
            // corresponds to  "$_path" fields which is always in ascending order
            builder.appendNumber(emptyFieldName, 1);
        }

        // Add an order corresponding to the next field of the Wildcard Index's key pattern.
        tassert(7354608,
                "All fields in a Wildcard Index's key pattern must be number values",
                field.isNumber());

        builder.appendNumber(emptyFieldName, field.numberInt());
    }

    return builder.obj();
}

/**
 * Retrieves from the wildcard index the set of multikey path metadata keys bounded by
 * 'indexBounds'. Returns the set of multikey paths represented by the keys.
 */
static std::set<FieldRef> getWildcardMultikeyPathSetHelper(OperationContext* opCtx,
                                                           const IndexCatalogEntry* entry,
                                                           const IndexBounds& indexBounds,
                                                           MultikeyMetadataAccessStats* stats) {
    const WildcardAccessMethod* wam =
        static_cast<const WildcardAccessMethod*>(entry->accessMethod());
    return writeConflictRetry(
        opCtx, "wildcard multikey path retrieval", NamespaceString(), [&]() -> std::set<FieldRef> {
            stats->numSeeks = 0;
            stats->keysExamined = 0;
            auto cursor = wam->newCursor(opCtx);

            constexpr int kForward = 1;
            const auto keyPattern = buildIndexBoundsKeyPattern(entry->descriptor()->keyPattern());
            IndexBoundsChecker checker(&indexBounds, keyPattern, kForward);
            IndexSeekPoint seekPoint;
            if (!checker.getStartSeekPoint(&seekPoint)) {
                return {};
            }

            std::set<FieldRef> multikeyPaths{};
            auto entry = cursor->seek(IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                seekPoint,
                wam->getSortedDataInterface()->getKeyStringVersion(),
                wam->getSortedDataInterface()->getOrdering(),
                kForward));


            ++stats->numSeeks;
            while (entry) {
                ++stats->keysExamined;

                switch (checker.checkKey(entry->key, &seekPoint)) {
                    case IndexBoundsChecker::VALID:
                        multikeyPaths.emplace(extractMultikeyPathFromIndexKey(*entry));
                        entry = cursor->next();
                        break;

                    case IndexBoundsChecker::MUST_ADVANCE:
                        ++stats->numSeeks;
                        entry =
                            cursor->seek(IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                seekPoint,
                                wam->getSortedDataInterface()->getKeyStringVersion(),
                                wam->getSortedDataInterface()->getOrdering(),
                                kForward));

                        break;

                    case IndexBoundsChecker::DONE:
                        entry = boost::none;
                        break;

                    default:
                        MONGO_UNREACHABLE;
                }
            }

            return multikeyPaths;
        });
}

std::vector<Interval> getMultikeyPathIndexIntervalsForField(FieldRef field) {
    std::vector<Interval> intervals;

    size_t pointIntervalPrefixParts = field.numParts();
    const size_t skipFirstFieldElement = 1;
    const auto numericPathComponents = field.getNumericPathComponents(skipFirstFieldElement);
    const auto hasNumericPathComponent = !numericPathComponents.empty();

    if (hasNumericPathComponent) {
        pointIntervalPrefixParts = *numericPathComponents.begin();
        invariant(pointIntervalPrefixParts > 0);
    }

    constexpr bool inclusive = true;
    constexpr bool exclusive = false;

    // Add a point interval for each path up to the first numeric path component. Field "a.b.c"
    // would produce point intervals ["a", "a"], ["a.b", "a.b"] and ["a.b.c", "a.b.c"]. Field
    // "a.b.0.c" would produce point intervals ["a", "a"] and ["a.b", "a.b"].
    for (size_t i = 1; i <= pointIntervalPrefixParts; ++i) {
        auto multikeyPath = field.dottedSubstring(0, i);
        intervals.push_back(IndexBoundsBuilder::makePointInterval(multikeyPath));
    }

    // If "field" contains a numeric path component then we add a range covering all subfields
    // of the non-numeric prefix.
    if (hasNumericPathComponent) {
        auto rangeBase = field.dottedSubstring(0, pointIntervalPrefixParts);
        StringBuilder multikeyPathStart;
        multikeyPathStart << rangeBase << ".";
        StringBuilder multikeyPathEnd;
        multikeyPathEnd << rangeBase << static_cast<char>('.' + 1);

        intervals.emplace_back(BSON("" << multikeyPathStart.str() << "" << multikeyPathEnd.str()),
                               inclusive,
                               exclusive);
    }

    return intervals;
}

/**
 * Returns the postion of the wildcard field inside the Wildcard Index's keyPattern and the
 * direction of 'IndexBounds' generated for querying multikey paths.
 */
static std::pair<size_t, bool> getWildcardFieldPosition(const BSONObj& keyPattern) {
    size_t pos = 0;
    for (const auto& field : keyPattern) {
        if (WildcardNames::isWildcardFieldName(field.fieldNameStringData())) {
            return {pos, field.numberInt() < 0};
        }
        ++pos;
    }

    tasserted(
        7354609,
        str::stream() << "Wildcard field is required in Wildcard's key pattern, but not found: "
                      << keyPattern);
}

/**
 * Returns the IndexBounds to retrieve multikey metadata keys for the given 'fieldSet'.
 */
static IndexBounds buildMetadataKeysIndexBounds(const BSONObj& keyPattern,
                                                const stdx::unordered_set<std::string>& fieldSet) {
    IndexBounds indexBounds;

    // Multikey metadata keys are stored in the following format:
    // '[MinKey]*, 1, <multikeypath>, [MinKey]*'.
    // A multikey metadata key is always prefixed with the number of MinKeys equal to the number of
    // regular fields in the index's keyPattern, then follows the number "1", followed by the string
    // value of the multikey path. At the end of the metadata key the number of MinKey equal to the
    // number of regular fields after the wildcard field is placed.
    // We build index bounds in the following 4 steps:
    // 1. Add the number of prefixed MinKey values, which is 0 or more.
    // 2. Add the number "1" which is always prefixed the multikey path value.
    // 3. Add the multikey path.
    // 4. Add the number of suffixed MinKey values, which is 0 or more.

    const auto [wildcardPosition, shouldReverse] = getWildcardFieldPosition(keyPattern);

    // Step 1. Add the number of prefixed MinKey values, which is 0 or more.
    for (size_t i = 0; i < wildcardPosition; ++i) {
        OrderedIntervalList preifixOil;
        preifixOil.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << MINKEY)));
        indexBounds.fields.push_back(std::move(preifixOil));
    }

    // Step 2. Add the number "1" which is always prefixed the multikey path value.
    OrderedIntervalList multikeyPathFlagOil;
    multikeyPathFlagOil.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    indexBounds.fields.push_back(std::move(multikeyPathFlagOil));

    // Step 3. Add the multikey path.
    OrderedIntervalList fieldNameOil;
    for (const auto& field : fieldSet) {
        auto intervals = getMultikeyPathIndexIntervalsForField(FieldRef(field));
        fieldNameOil.intervals.insert(fieldNameOil.intervals.end(),
                                      std::make_move_iterator(intervals.begin()),
                                      std::make_move_iterator(intervals.end()));
    }

    // IndexBoundsBuilder::unionize() sorts the OrderedIntervalList allowing for in order index
    // traversal.
    IndexBoundsBuilder::unionize(&fieldNameOil);
    if (shouldReverse) {
        fieldNameOil.reverse();
    }
    indexBounds.fields.push_back(std::move(fieldNameOil));

    // Step 4. Add the number of suffixed MinKey values, which is 0 or more.
    const size_t keyPatternNFields = static_cast<size_t>(keyPattern.nFields());
    for (size_t i = wildcardPosition + 1; i < keyPatternNFields; ++i) {
        OrderedIntervalList suffixOil;
        suffixOil.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << MINKEY)));
        indexBounds.fields.push_back(std::move(suffixOil));
    }

    return indexBounds;
}

std::set<FieldRef> getWildcardMultikeyPathSet(OperationContext* opCtx,
                                              const IndexCatalogEntry* entry,
                                              const stdx::unordered_set<std::string>& fieldSet,
                                              MultikeyMetadataAccessStats* stats) {
    tassert(7354610, "stats must be non-null", stats);

    const auto& indexBounds =
        buildMetadataKeysIndexBounds(entry->descriptor()->keyPattern(), fieldSet);
    return getWildcardMultikeyPathSetHelper(opCtx, entry, indexBounds, stats);
}

/**
 * Return key range to retrieve all multikey metadata keys.
 */
static std::pair<BSONObj, BSONObj> buildMetadataKeyRange(const BSONObj& keyPattern) {
    static constexpr StringData emptyFieldName = ""_sd;

    BSONObjBuilder rangeBeginBuilder{};
    BSONObjBuilder rangeEndBuilder{};
    for (const auto& field : keyPattern) {
        if (WildcardNames::isWildcardFieldName(field.fieldNameStringData())) {
            rangeBeginBuilder.appendNumber(emptyFieldName, 1);
            rangeEndBuilder.appendNumber(emptyFieldName, 1);
            break;
        } else {
            rangeBeginBuilder.appendMinKey(emptyFieldName);
            rangeEndBuilder.appendMinKey(emptyFieldName);
        }
    }

    rangeBeginBuilder.appendMinKey(emptyFieldName);
    rangeEndBuilder.appendMaxKey(emptyFieldName);
    return std::make_pair(rangeBeginBuilder.obj(), rangeEndBuilder.obj());
}

std::set<FieldRef> getWildcardMultikeyPathSet(OperationContext* opCtx,
                                              const IndexCatalogEntry* entry,
                                              MultikeyMetadataAccessStats* stats) {
    return writeConflictRetry(opCtx, "wildcard multikey path retrieval", NamespaceString(), [&]() {
        tassert(7354611, "stats must be non-null", stats);
        stats->numSeeks = 0;
        stats->keysExamined = 0;

        const WildcardAccessMethod* wam =
            static_cast<const WildcardAccessMethod*>(entry->accessMethod());
        auto cursor = wam->newCursor(opCtx);

        const auto [metadataKeyRangeBegin, metadataKeyRangeEnd] =
            buildMetadataKeyRange(entry->descriptor()->keyPattern());

        constexpr bool inclusive = true;
        cursor->setEndPosition(metadataKeyRangeEnd, inclusive);

        auto keyStringForSeek = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
            metadataKeyRangeBegin,
            wam->getSortedDataInterface()->getKeyStringVersion(),
            wam->getSortedDataInterface()->getOrdering(),
            true, /* forward */
            inclusive);
        auto entry = cursor->seek(keyStringForSeek);
        ++stats->numSeeks;

        // Iterate the cursor, copying the multikey paths into an in-memory set.
        std::set<FieldRef> multikeyPaths{};
        while (entry) {
            ++stats->keysExamined;
            multikeyPaths.emplace(extractMultikeyPathFromIndexKey(*entry));

            entry = cursor->next();
        }

        return multikeyPaths;
    });
}

}  // namespace mongo
