// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/metadata_compressor.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
using namespace std::literals::string_view_literals;

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

        if (fieldName == "start"sv || fieldName == "end"sv) {
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

            if (lvl2FieldName == "start"sv || lvl2FieldName == "end"sv) {
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
