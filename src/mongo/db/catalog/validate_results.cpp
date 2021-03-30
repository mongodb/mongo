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

namespace mongo {

void ValidateResults::appendToResultObj(BSONObjBuilder* resultObj, bool debugging) const {
    resultObj->appendBool("valid", valid);
    resultObj->appendBool("repaired", repaired);
    if (readTimestamp) {
        resultObj->append("readTimestamp", readTimestamp.get());
    }

    static constexpr std::size_t kMaxErrorWarningSizeBytes = 2 * 1024 * 1024;
    auto appendRangeSizeLimited = [resultObj](StringData fieldName, const auto& values) {
        std::size_t usedSize = 0;
        BSONArrayBuilder arr(resultObj->subarrayStart(fieldName));
        for (auto it = values.begin(), end = values.end();
             it != end && usedSize < kMaxErrorWarningSizeBytes;
             ++it) {
            arr.append(*it);
            usedSize += it->size();
        }
    };

    appendRangeSizeLimited("warnings"_sd, warnings);
    appendRangeSizeLimited("errors"_sd, errors);

    resultObj->append("extraIndexEntries", extraIndexEntries);
    resultObj->append("missingIndexEntries", missingIndexEntries);

    // Need to convert RecordId to a printable type.
    BSONArrayBuilder builder;
    for (const RecordId& corruptRecord : corruptRecords) {
        BSONObjBuilder objBuilder;
        corruptRecord.serializeToken("", &objBuilder);
        builder.append(objBuilder.done().firstElement());
    }
    resultObj->append("corruptRecords", builder.arr());

    if (repaired || debugging) {
        resultObj->appendNumber("numRemovedCorruptRecords", numRemovedCorruptRecords);
        resultObj->appendNumber("numRemovedExtraIndexEntries", numRemovedExtraIndexEntries);
        resultObj->appendNumber("numInsertedMissingIndexEntries", numInsertedMissingIndexEntries);
    }
}
}  // namespace mongo
