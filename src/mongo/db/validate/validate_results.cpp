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

#include "mongo/db/validate/validate_results.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bson_utf8.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/util/namespace_string_util.h"

namespace mongo {

namespace {

// Up to 1MB of "inconsistency" errors will be kept, i.e. missing/extra index fields.
const int kMaxIndexInconsistencySize = 1 * 1024 * 1024;
// Reserving 4MB for index details' error and warning messages.
static constexpr std::size_t kMaxIndexDetailsSizeBytes = 4 * 1024 * 1024;

struct LargestObjsPopFirstCmp {
    bool operator()(const BSONObj& l, const BSONObj& r) {
        return l.objsize() < r.objsize();
    }
};

// Helper for adding |obj| to a list of bson objs, where only the smallest objects up to a limit
// will be kept. At least 1 object will always be kept. Returns true if an element was removed.
bool addWithSizeLimit(BSONObj obj, std::vector<BSONObj>& list, size_t& usedBytes) {
    usedBytes += static_cast<size_t>(obj.objsize());
    list.push_back(std::move(obj));
    std::push_heap(list.begin(), list.end(), LargestObjsPopFirstCmp{});
    if (usedBytes <= kMaxIndexInconsistencySize || list.size() <= 1) {
        return false;
    }
    std::pop_heap(list.begin(), list.end(), LargestObjsPopFirstCmp{});
    usedBytes -= list.back().objsize();
    list.pop_back();
    return true;
}


// Builds an array inside output containing the entries up to a given max size per entry.
void buildFixedSizedArray(BSONObjBuilder& output,
                          const std::string& fieldname,
                          const StringSet& entries,
                          size_t maxSizePerEntry) {

    std::size_t usedSize = 0;
    BSONArrayBuilder arr(output.subarrayStart(fieldname));

    for (const auto& value : entries) {
        if (usedSize >= maxSizePerEntry) {
            return;
        }
        arr.append(value);
        usedSize += value.size();
    }
}

}  // namespace

void ValidateResults::addExtraIndexEntry(BSONObj entry) {
    if (addWithSizeLimit(std::move(entry), _extraIndexEntries, _extraIndexEntriesUsedBytes)) {
        addError("Not all extra index entry inconsistencies are listed due to size limitations.",
                 false);
    }
}

void ValidateResults::addMissingIndexEntry(BSONObj entry) {
    if (addWithSizeLimit(std::move(entry), _missingIndexEntries, _missingIndexEntriesUsedBytes)) {
        addError("Not all missing index entry inconsistencies are listed due to size limitations.",
                 false);
    }
}

void ValidateResults::setRepairMode(CollectionValidation::RepairMode mode) {
    switch (mode) {
        case CollectionValidation::RepairMode::kNone:
            _repairMode = "None";
            break;
        case CollectionValidation::RepairMode::kFixErrors:
            _repairMode = "FixErrors";
            break;
        case CollectionValidation::RepairMode::kAdjustMultikey:
            _repairMode = "AdjustMultikey";
            break;
    }
}

void ValidateResults::appendToResultObj(BSONObjBuilder* resultObj,
                                        bool debugging,
                                        const SerializationContext& sc) const {
    resultObj->appendBool("valid", isValid());
    resultObj->appendBool("repaired", getRepaired());
    resultObj->append("repairMode", getRepairMode());
    if (getReadTimestamp()) {
        resultObj->append("readTimestamp", getReadTimestamp().value());
    }
    if (_nss.has_value()) {
        resultObj->append("ns", NamespaceStringUtil::serialize(_nss.value(), sc));
    }
    if (_uuid.has_value()) {
        _uuid->appendToBuilder(resultObj, "uuid");
    }

    static constexpr std::size_t kMaxErrorWarningSizeBytes = 2 * 1024 * 1024;
    auto appendRangeSizeLimited = [&](StringData fieldName, auto valueGetter) {
        std::size_t usedSize = 0;
        BSONArrayBuilder arr(resultObj->subarrayStart(fieldName));
        for (const auto& value : valueGetter(*this)) {
            if (usedSize >= kMaxErrorWarningSizeBytes) {
                return;
            }
            arr.append(value);
            usedSize += value.size();
        }
        for (const auto& idxDetails : getIndexResultsMap()) {
            if (usedSize >= kMaxErrorWarningSizeBytes) {
                return;
            }
            if (valueGetter(idxDetails.second).empty()) {
                continue;
            }
            std::string message = str::stream()
                << "Found " << fieldName << " in " << idxDetails.first;
            usedSize += message.size();
            arr.append(std::move(message));
        }
    };

    appendRangeSizeLimited("warnings"_sd,
                           [](const auto& results) { return results.getWarnings(); });
    appendRangeSizeLimited("errors"_sd, [](const auto& results) { return results.getErrors(); });

    // appendScrubInvalidUTF8Values recursively ranges through each BSONObj and scrubs invalid UTF-8
    // string data in the BSONObj with"\xef\xbf\xbd", which is the UTF-8 encoding of the replacement
    // character U+FFFD.
    // https://en.wikipedia.org/wiki/Specials_(Unicode_block)#Replacement_character
    auto appendScrubInvalidUTF8Values = [&](StringData fieldName, const auto& values) {
        BSONArrayBuilder arr(resultObj->subarrayStart(fieldName));
        for (auto&& v : values) {
            arr.append(checkAndScrubInvalidUTF8(v));
        }
    };

    appendScrubInvalidUTF8Values("extraIndexEntries"_sd, getExtraIndexEntries());
    appendScrubInvalidUTF8Values("missingIndexEntries"_sd, getMissingIndexEntries());
    if (_numInvalidDocuments.has_value()) {
        resultObj->appendNumber("nInvalidDocuments", _numInvalidDocuments.value());
    }
    if (_numNonCompliantDocuments.has_value()) {
        resultObj->appendNumber("nNonCompliantDocuments", _numNonCompliantDocuments.value());
    }
    if (_numRecords.has_value()) {
        resultObj->appendNumber("nrecords", _numRecords.value());
    }

    // Need to convert RecordId to a printable type.
    BSONArrayBuilder builder;
    for (const RecordId& corruptRecord : getCorruptRecords()) {
        BSONObjBuilder objBuilder;
        corruptRecord.serializeToken("", &objBuilder);
        builder.append(objBuilder.done().firstElement());
    }
    resultObj->append("corruptRecords", builder.arr());

    // Report detailed index validation results for validated indexes.
    BSONObjBuilder keysPerIndex;
    BSONObjBuilder indexDetails;
    int nIndexes = getIndexResultsMap().size();

    // Each list has a size based on the number of indexes, split between error/warning fields.
    size_t maxSizePerEntry = (kMaxIndexDetailsSizeBytes / std::max(nIndexes, 1)) / 2;
    for (auto& [indexName, ivr] : getIndexResultsMap()) {
        BSONObjBuilder bob(indexDetails.subobjStart(indexName));
        bob.appendBool("valid", ivr.isValid());
        bob.append("spec", ivr.getSpec());

        if (!ivr.getWarnings().empty()) {
            buildFixedSizedArray(bob, "warnings", ivr.getWarnings(), maxSizePerEntry);
        }
        if (!ivr.getErrors().empty()) {
            buildFixedSizedArray(bob, "errors", ivr.getErrors(), maxSizePerEntry);
        }

        keysPerIndex.appendNumber(indexName, static_cast<long long>(ivr.getKeysTraversed()));
    }
    resultObj->append("nIndexes", nIndexes);
    resultObj->append("keysPerIndex", keysPerIndex.done());
    resultObj->append("indexDetails", indexDetails.done());

    if (getRepaired() || debugging) {
        resultObj->appendNumber("numRemovedCorruptRecords", getNumRemovedCorruptRecords());
        resultObj->appendNumber("numRemovedExtraIndexEntries", getNumRemovedExtraIndexEntries());
        resultObj->appendNumber("numInsertedMissingIndexEntries",
                                getNumInsertedMissingIndexEntries());
        resultObj->appendNumber("numDocumentsMovedToLostAndFound",
                                getNumDocumentsMovedToLostAndFound());
        resultObj->appendNumber("numOutdatedMissingIndexEntry", getNumOutdatedMissingIndexEntry());
    }
}
}  // namespace mongo
