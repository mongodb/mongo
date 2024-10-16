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

#include "mongo/db/catalog/validate_results.h"

#include <cstddef>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/util/namespace_string_util.h"

namespace mongo {

void ValidateResults::appendToResultObj(BSONObjBuilder* resultObj,
                                        bool debugging,
                                        const SerializationContext& sc) const {
    resultObj->appendBool("valid", isValid());
    resultObj->appendBool("repaired", getRepaired());
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

    resultObj->append("extraIndexEntries", getExtraIndexEntries());
    resultObj->append("missingIndexEntries", getMissingIndexEntries());
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
    for (auto& [indexName, ivr] : getIndexResultsMap()) {
        BSONObjBuilder bob(indexDetails.subobjStart(indexName));
        bob.appendBool("valid", ivr.isValid());

        if (!ivr.getWarnings().empty()) {
            bob.append("warnings", ivr.getWarnings());
        }

        if (!ivr.getErrors().empty()) {
            bob.append("errors", ivr.getErrors());
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
