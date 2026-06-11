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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/validate/bson_utf8.h"
#include "mongo/db/validate/validate_options.h"

namespace mongo {

namespace {

// Up to 1MB of "inconsistency" errors will be kept, i.e. missing/extra index fields.
static constexpr int kMaxIndexInconsistencySize = 1 * 1024 * 1024;
// Reserving 4MB for index details' error and warning messages.
static constexpr size_t kMaxIndexDetailsSizeBytes = 4 * 1024 * 1024;

struct LargestObjsPopFirstCmp {
    bool operator()(const BSONObj& l, const BSONObj& r) {
        return l.objsize() < r.objsize();
    }
};

// Helper for adding |obj| to a list of bson objs, where only the smallest objects up to a limit
// will be kept. At least 1 object will always be kept. Returns true if an element was removed.
bool addWithSizeLimit(const BSONObj& obj, std::vector<BSONObj>& dst, size_t& usedBytes) {
    invariant(obj.objsize() > 0);
    usedBytes += static_cast<size_t>(obj.objsize());
    dst.push_back(std::move(obj));
    std::push_heap(dst.begin(), dst.end(), LargestObjsPopFirstCmp{});
    if (usedBytes <= kMaxIndexInconsistencySize || dst.size() <= 1) {
        return false;
    }
    std::pop_heap(dst.begin(), dst.end(), LargestObjsPopFirstCmp{});
    usedBytes -= dst.back().objsize();
    dst.pop_back();
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

void ValidateResultsIf::_mergeBase(const ValidateResultsIf& other) {
    // Expect continuation to require all-of
    _continueValidation &= other._continueValidation;
    // fatalError is any-of
    _fatalError |= other._fatalError;
    _errors.insert(other._errors.begin(), other._errors.end());
    _warnings.insert(other._warnings.begin(), other._warnings.end());
}

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

void ValidateResults::setRepairMode(collection_validation::RepairMode mode) {
    switch (mode) {
        case collection_validation::RepairMode::kNone:
            _repairMode = "None";
            break;
        case collection_validation::RepairMode::kFixErrors:
            _repairMode = "FixErrors";
            break;
        case collection_validation::RepairMode::kAdjustMultikey:
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
    if (_fastCountType.has_value()) {
        resultObj->append("fastCountType", toString(_fastCountType.value()));
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

    if (_collectionHash.has_value()) {
        resultObj->append("all", _collectionHash->toHexString());
    }

    if (_metadataHash.has_value()) {
        resultObj->append("metadata", _metadataHash->toHexString());
    }

    if (_partialHashes.has_value()) {
        BSONObjBuilder bob(resultObj->subobjStart("partial"));
        for (const auto& [prefix, hashAndCount] : *_partialHashes) {
            bob.append(prefix,
                       BSON("hash" << hashAndCount.first << "count" << hashAndCount.second));
        }
    }

    if (_revealedIds.has_value()) {
        BSONObjBuilder bob(resultObj->subobjStart("revealedIds"));
        for (const auto& [prefix, ids] : *_revealedIds) {
            bob.append(prefix, ids);
        }
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
        bob.appendBool("isMultikey", ivr.isMultikey());

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

void ValidateResults::merge(const ValidateResults& other) {
    uassert(12759600, "Merged results must have the same UUID", _uuid == other._uuid);
    uassert(12759601, "Merged results must have the same Namespace", _nss == other._nss);
    uassert(12759602,
            "Merged ValidateResults must have the same repair mode",
            _repairMode == other._repairMode);
    uassert(12759603,
            "Merging results requires identical snapshot timestamps for each partial result",
            _readTimestamp == other._readTimestamp);
    uassert(12759604,
            "Merging partial hash results from hash drill down is not supported",
            !_partialHashes && !other._partialHashes && !_revealedIds && !other._revealedIds);

    ValidateResults tmp = *this;

    // Per-index results. Indexes absent from this result are copied in wholesale; shared indexes
    // have their errors, warnings and counters combined via the public interface. The per-index
    // continuation/fatal flags have no public setter and aren't produced during the record-store
    // pass, so they are intentionally left untouched here.
    // This occurs first in the merge as the index keys must have an identical spec, so there exists
    // the possibility of an early exit if the specs are mismatched.
    for (const auto& [indexName, otherIvr] : other._indexResultsMap) {
        auto [it, inserted] = tmp._indexResultsMap.try_emplace(indexName);
        if (inserted) {
            it->second = otherIvr;
            continue;
        }
        auto& ivr = it->second;
        ivr.getErrorsUnsafe()->insert(otherIvr.getErrors().begin(), otherIvr.getErrors().end());
        ivr.getWarningsUnsafe()->insert(otherIvr.getWarnings().begin(),
                                        otherIvr.getWarnings().end());
        ivr.addKeysTraversed(otherIvr.getKeysTraversed());
        ivr.addKeysRemovedFromRecordStore(otherIvr.getKeysRemovedFromRecordStore());
        ivr.setHasStructuralDamage(ivr.hasStructuralDamage() || otherIvr.hasStructuralDamage());
        ivr.setIsMultikey(ivr.isMultikey() || otherIvr.isMultikey());
        uassert(12759605,
                "Merging index results with conflicting specs for index: " + indexName,
                (ivr.getSpec().isEmpty() && otherIvr.getSpec().isEmpty()) ||
                    ivr.getSpec().binaryEqual(otherIvr.getSpec()));
    }

    for (const auto& eia : other._extraIndexEntries) {
        tmp.addExtraIndexEntry(eia);
    }
    for (const auto& mie : other._missingIndexEntries) {
        tmp.addMissingIndexEntry(mie);
    }

    tmp._corruptRecords.insert(
        tmp._corruptRecords.end(), other._corruptRecords.begin(), other._corruptRecords.end());

    tmp._repaired = tmp._repaired || other._repaired;
    tmp._hasStructuralDamage = tmp._hasStructuralDamage || other._hasStructuralDamage;

    // Identity-valued fields are the same for every chunk of a single collection; adopt them if
    // this result hasn't recorded them yet.
    if (!tmp._fastCountType) {
        tmp._fastCountType = other._fastCountType;
    }

    // Document and record counters are additive. The optionals only become engaged once a chunk
    // has progressed far enough to compute them, so treat a missing value as zero.
    auto addOptional = [](boost::optional<long long>& dst, const boost::optional<long long>& src) {
        if (src) {
            dst = dst.value_or(0) + *src;
        }
    };
    addOptional(tmp._numInvalidDocuments, other._numInvalidDocuments);
    addOptional(tmp._numNonCompliantDocuments, other._numNonCompliantDocuments);
    addOptional(tmp._numRecords, other._numRecords);

    tmp._numRemovedCorruptRecords += other._numRemovedCorruptRecords;
    tmp._numRemovedExtraIndexEntries += other._numRemovedExtraIndexEntries;
    tmp._numInsertedMissingIndexEntries += other._numInsertedMissingIndexEntries;
    tmp._numDocumentsMovedToLostAndFound += other._numDocumentsMovedToLostAndFound;
    tmp._numOutdatedMissingIndexEntry += other._numOutdatedMissingIndexEntry;

    tmp._recordTimestamps.insert(other._recordTimestamps.begin(), other._recordTimestamps.end());

    // The collection and partial hashes are order-independent XORs of per-record SHA256 blocks and
    // must be combined at the SHA256Block level before being stringified, so merge() only adopts a
    // value when this result doesn't already have one.
    if (!tmp._collectionHash) {
        tmp._collectionHash = other._collectionHash;
    } else if (other._collectionHash) {
        tmp._collectionHash->xorInline(*other._collectionHash);
    }

    if (!tmp._metadataHash) {
        tmp._metadataHash = other._metadataHash;
    } else if (other._metadataHash) {
        tmp._metadataHash->xorInline(*other._metadataHash);
    }

    tmp._mergeBase(other);

    std::swap(*this, tmp);
}

}  // namespace mongo
