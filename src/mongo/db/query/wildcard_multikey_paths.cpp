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

#include "mongo/platform/basic.h"

#include "mongo/db/query/wildcard_multikey_paths.h"

#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/record_id_helpers.h"

namespace mongo {

/**
 * Extracts the multikey path from a metadata key stored within a wildcard index.
 */
static FieldRef extractMultikeyPathFromIndexKey(const IndexKeyEntry& entry) {
    invariant(record_id_helpers::isReserved(entry.loc));
    invariant(
        !entry.loc.isLong() ||
        entry.loc ==
            record_id_helpers::reservedIdFor(
                record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::Long));
    invariant(
        !entry.loc.isStr() ||
        entry.loc ==
            record_id_helpers::reservedIdFor(
                record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::String));

    // Validate that the first piece of the key is the integer 1.
    BSONObjIterator iter(entry.key);
    invariant(iter.more());
    const auto firstElem = iter.next();
    invariant(firstElem.isNumber());
    invariant(firstElem.numberInt() == 1);
    invariant(iter.more());

    // Extract the path from the second piece of the key.
    const auto secondElem = iter.next();
    invariant(!iter.more());
    invariant(secondElem.type() == BSONType::String);

    return FieldRef(secondElem.valueStringData());
}

/**
 * Retrieves from the wildcard index the set of multikey path metadata keys bounded by
 * 'indexBounds'. Returns the set of multikey paths represented by the keys.
 */
static std::set<FieldRef> getWildcardMultikeyPathSetHelper(const WildcardAccessMethod* wam,
                                                           OperationContext* opCtx,
                                                           const IndexBounds& indexBounds,
                                                           MultikeyMetadataAccessStats* stats) {
    return writeConflictRetry(
        opCtx, "wildcard multikey path retrieval", "", [&]() -> std::set<FieldRef> {
            stats->numSeeks = 0;
            stats->keysExamined = 0;
            auto cursor = wam->newCursor(opCtx);

            constexpr int kForward = 1;
            const auto keyPattern = BSON("" << 1 << "" << 1);
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

std::set<FieldRef> getWildcardMultikeyPathSet(const WildcardAccessMethod* wam,
                                              OperationContext* opCtx,
                                              const stdx::unordered_set<std::string>& fieldSet,
                                              MultikeyMetadataAccessStats* stats) {
    invariant(stats);
    IndexBounds indexBounds;

    // Multikey metadata keys are stored with the number "1" in the first position of the index to
    // differentiate them from user-data keys, which contain a string representing the path.
    OrderedIntervalList multikeyPathFlagOil;
    multikeyPathFlagOil.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    indexBounds.fields.push_back(std::move(multikeyPathFlagOil));

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
    indexBounds.fields.push_back(std::move(fieldNameOil));

    return getWildcardMultikeyPathSetHelper(wam, opCtx, indexBounds, stats);
}

std::set<FieldRef> getWildcardMultikeyPathSet(const WildcardAccessMethod* wam,
                                              OperationContext* opCtx,
                                              MultikeyMetadataAccessStats* stats) {
    return writeConflictRetry(opCtx, "wildcard multikey path retrieval", "", [&]() {
        invariant(stats);
        stats->numSeeks = 0;
        stats->keysExamined = 0;

        auto cursor = wam->newCursor(opCtx);

        // All of the keys storing multikeyness metadata are prefixed by a value of 1. Establish
        // an index cursor which will scan this range.
        const BSONObj metadataKeyRangeBegin = BSON("" << 1 << "" << MINKEY);
        const BSONObj metadataKeyRangeEnd = BSON("" << 1 << "" << MAXKEY);

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
