/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/ftdc/metadata_compressor.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

namespace {
/*
 * Returns whether the sample object has the same sequence of field names as the ref object.
 */
bool haveSameFields(BSONObj ref, BSONObj sample) {
    bool match = true;
    BSONObjIterator refItr(ref);
    BSONObjIterator sampleItr(sample);
    while (refItr.more() && sampleItr.more() && match) {
        match &= (refItr.next().fieldNameStringData() == sampleItr.next().fieldNameStringData());
    }
    match &= !(sampleItr.more() || refItr.more());
    return match;
};
}  // namespace

/*
 * This expects the input sample document to have the following layout:
 * {
 *   start: Date_t,
 *   lvl1: {
 *      start: Date_t,
 *      lvl2field1: Value,
 *      ...
 *      end: Date_t,
 *   },
 *   lvl2: ...,
 *   end: Date_t,
 * }
 * where "lvl[N]" is a placeholder for a command name, and "lvl[N]field[M]"
 * is a placeholder for a field name in the command response.
 */
boost::optional<BSONObj> FTDCMetadataCompressor::addSample(const BSONObj& sample) {
    if (!haveSameFields(_referenceDoc, sample)) {
        _reset(sample);
        return sample;
    }

    BSONObjBuilder deltaDocBuilder;
    bool hasChanges = false;

    BSONObjIterator sampleItr(sample);
    BSONObjIterator refItr(_referenceDoc);

    while (refItr.more()) {
        auto refElement = refItr.next();
        auto sampleElement = sampleItr.next();
        auto fieldName = sampleElement.fieldNameStringData();

        if (fieldName == "start"_sd || fieldName == "end"_sd) {
            dassert(sampleElement.type() == BSONType::date);
            deltaDocBuilder.append(sampleElement);
            continue;
        }

        dassert(sampleElement.type() == BSONType::object);
        dassert(refElement.type() == BSONType::object);

        auto sampleSubObj = sampleElement.Obj();
        auto refSubObj = refElement.Obj();

        if (!haveSameFields(refSubObj, sampleSubObj)) {
            _reset(sample);
            return sample;
        }

        std::vector<BSONElement> deltaElements;
        bool lvl2HasChanges = false;

        BSONObjIterator sampleLvl2Itr(sampleSubObj);
        BSONObjIterator refLvl2Itr(refSubObj);

        while (refLvl2Itr.more()) {
            auto refLvl2Element = refLvl2Itr.next();
            auto sampleLvl2Element = sampleLvl2Itr.next();
            auto lvl2FieldName = sampleLvl2Element.fieldNameStringData();

            if (lvl2FieldName == "start"_sd || lvl2FieldName == "end"_sd) {
                dassert(sampleLvl2Element.type() == BSONType::date);
                deltaElements.push_back(sampleLvl2Element);
                continue;
            }

            if (sampleLvl2Element.woCompare(refLvl2Element) != 0) {
                deltaElements.push_back(sampleLvl2Element);
                lvl2HasChanges = true;
            }
        }

        if (lvl2HasChanges) {
            BSONObjBuilder subBob = deltaDocBuilder.subobjStart(fieldName);
            for (auto& element : deltaElements) {
                subBob.append(element);
            }
            hasChanges = true;
        }
    }

    if (hasChanges) {
        _referenceDoc = sample;
        _deltaCount++;
        return deltaDocBuilder.obj();
    }

    return boost::none;
}

void FTDCMetadataCompressor::reset() {
    _reset(BSONObj());
}

void FTDCMetadataCompressor::_reset(const BSONObj& newReference) {
    _deltaCount = 0;
    _referenceDoc = newReference;
}

}  // namespace mongo
