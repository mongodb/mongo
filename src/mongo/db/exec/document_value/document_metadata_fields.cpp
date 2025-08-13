/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document_metadata_fields.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/string_map.h"

#include <ostream>
#include <vector>

namespace mongo {
using MetaType = DocumentMetadataFields::MetaType;

namespace {
Value missingToNull(Value maybeMissing) {
    return maybeMissing.missing() ? Value(BSONNULL) : maybeMissing;
}

static const std::string textScoreName = "textScore";
static const std::string randValName = "randVal";
static const std::string searchScoreName = "searchScore";
static const std::string searchHighlightsName = "searchHighlights";
static const std::string geoNearDistanceName = "geoNearDistance";
static const std::string geoNearPointName = "geoNearPoint";
static const std::string recordIdName = "recordId";
static const std::string indexKeyName = "indexKey";
static const std::string sortKeyName = "sortKey";
static const std::string searchScoreDetailsName = "searchScoreDetails";
static const std::string searchRootDocumentIdName = "searchRootDocumentId";
static const std::string searchSequenceTokenName = "searchSequenceToken";
static const std::string timeseriesBucketMinTimeName = "timeseriesBucketMinTime";
static const std::string timeseriesBucketMaxTimeName = "timeseriesBucketMaxTime";
static const std::string vectorSearchScoreName = "vectorSearchScore";
static const std::string scoreName = "score";
static const std::string scoreDetailsName = "scoreDetails";

// This field ("value") is extracted from the 'scoreDetails' Document to set the 'score' field too.
static const std::string scoreDetailsScoreField = "value";
static const std::string streamName = "stream";

static const std::string changeStreamControlEventName = "changeStreamControlEvent";

static const StringMap<MetaType> kMetaNameToMetaType = {
    {scoreName, MetaType::kScore},
    {vectorSearchScoreName, MetaType::kVectorSearchScore},
    {geoNearDistanceName, MetaType::kGeoNearDist},
    {geoNearPointName, MetaType::kGeoNearPoint},
    {indexKeyName, MetaType::kIndexKey},
    {randValName, MetaType::kRandVal},
    {recordIdName, MetaType::kRecordId},
    {searchHighlightsName, MetaType::kSearchHighlights},
    {searchScoreName, MetaType::kSearchScore},
    {searchScoreDetailsName, MetaType::kSearchScoreDetails},
    {searchSequenceTokenName, MetaType::kSearchSequenceToken},
    {sortKeyName, MetaType::kSortKey},
    {textScoreName, MetaType::kTextScore},
    {timeseriesBucketMinTimeName, MetaType::kTimeseriesBucketMinTime},
    {timeseriesBucketMaxTimeName, MetaType::kTimeseriesBucketMaxTime},
    {scoreDetailsName, MetaType::kScoreDetails},
    {searchRootDocumentIdName, MetaType::kSearchRootDocumentId},
    {streamName, MetaType::kStream},
    {changeStreamControlEventName, MetaType::kChangeStreamControlEvent},
};

// NOLINTNEXTLINE needs audit
static const std::unordered_map<MetaType, StringData> kMetaTypeToMetaName = {
    {MetaType::kScore, scoreName},
    {MetaType::kVectorSearchScore, vectorSearchScoreName},
    {MetaType::kGeoNearDist, geoNearDistanceName},
    {MetaType::kGeoNearPoint, geoNearPointName},
    {MetaType::kIndexKey, indexKeyName},
    {MetaType::kRandVal, randValName},
    {MetaType::kRecordId, recordIdName},
    {MetaType::kSearchHighlights, searchHighlightsName},
    {MetaType::kSearchScore, searchScoreName},
    {MetaType::kSearchScoreDetails, searchScoreDetailsName},
    {MetaType::kSearchSequenceToken, searchSequenceTokenName},
    {MetaType::kSortKey, sortKeyName},
    {MetaType::kTextScore, textScoreName},
    {MetaType::kTimeseriesBucketMinTime, timeseriesBucketMinTimeName},
    {MetaType::kTimeseriesBucketMaxTime, timeseriesBucketMaxTimeName},
    {MetaType::kScoreDetails, scoreDetailsName},
    {MetaType::kSearchRootDocumentId, searchRootDocumentIdName},
    {MetaType::kStream, streamName},
    {MetaType::kChangeStreamControlEvent, changeStreamControlEventName},
};
}  // namespace

DocumentMetadataFields::DocumentMetadataFields(const DocumentMetadataFields& other)
    : _holder(other._holder ? std::make_unique<MetadataHolder>(*other._holder) : nullptr) {}

DocumentMetadataFields& DocumentMetadataFields::operator=(const DocumentMetadataFields& other) {
    if (this != &other) {
        _holder = other._holder ? std::make_unique<MetadataHolder>(*other._holder) : nullptr;
        _modified = true;
    }
    return *this;
}

DocumentMetadataFields::DocumentMetadataFields(DocumentMetadataFields&& other)
    : _holder(std::move(other._holder)) {}

DocumentMetadataFields& DocumentMetadataFields::operator=(DocumentMetadataFields&& other) {
    _holder = std::move(other._holder);
    _modified = true;
    return *this;
}

MetaType DocumentMetadataFields::parseMetaType(StringData name) {
    const auto iter = kMetaNameToMetaType.find(name);
    uassert(17308, "Unsupported $meta field: " + name, iter != kMetaNameToMetaType.end());
    return iter->second;
}

StringData DocumentMetadataFields::serializeMetaType(MetaType type) {
    const auto nameIter = kMetaTypeToMetaName.find(type);
    tassert(9733900,
            str::stream() << "No name found for meta type: " << type,
            nameIter != kMetaTypeToMetaName.end());
    return nameIter->second;
}

void DocumentMetadataFields::setMetaFieldFromValue(MetaType type, Value val) {
    auto assertType = [&](BSONType typeRequested) {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "Meta field '" << serializeMetaType(type) << "' requires "
                              << typeName(typeRequested) << " type, found "
                              << typeName(val.getType()),
                val.getType() == typeRequested);
    };
    auto assertNumeric = [&]() {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "Meta field '" << serializeMetaType(type)
                              << "' requires numeric type, found " << typeName(val.getType()),
                val.numeric());
    };

    switch (type) {
        case DocumentMetadataFields::kGeoNearDist:
            assertNumeric();
            setGeoNearDistance(val.getDouble());
            break;
        case DocumentMetadataFields::kGeoNearPoint:
            setGeoNearPoint(val);
            break;
        case DocumentMetadataFields::kIndexKey:
            assertType(BSONType::object);
            setIndexKey(val.getDocument().toBson());
            break;
        case DocumentMetadataFields::kRandVal:
            assertNumeric();
            setRandVal(val.getDouble());
            break;
        case DocumentMetadataFields::kRecordId:
            assertNumeric();
            setRecordId(RecordId(val.getLong()));
            break;
        case DocumentMetadataFields::kSearchHighlights:
            setSearchHighlights(val);
            break;
        case DocumentMetadataFields::kSearchScore:
            assertNumeric();
            setSearchScore(val.getDouble());
            break;
        case DocumentMetadataFields::kTextScore:
            assertNumeric();
            setTextScore(val.getDouble());
            break;
        case DocumentMetadataFields::kSearchScoreDetails:
            assertType(BSONType::object);
            setSearchScoreDetails(val.getDocument().toBson());
            break;
        case DocumentMetadataFields::kSearchRootDocumentId:
            setSearchRootDocumentId(val);
            break;
        case DocumentMetadataFields::kTimeseriesBucketMinTime:
            assertType(BSONType::date);
            setTimeseriesBucketMinTime(val.getDate());
            break;
        case DocumentMetadataFields::kTimeseriesBucketMaxTime:
            assertType(BSONType::date);
            setTimeseriesBucketMaxTime(val.getDate());
            break;
        case DocumentMetadataFields::kSearchSortValues:
            assertType(BSONType::object);
            setSearchSortValues(val.getDocument().toBson());
            break;
        case DocumentMetadataFields::kSearchSequenceToken:
            setSearchSequenceToken(val);
            break;
        case DocumentMetadataFields::kVectorSearchScore:
            assertNumeric();
            setVectorSearchScore(val.getDouble());
            break;
        case DocumentMetadataFields::kScore:
            assertNumeric();
            setScore(val.getDouble());
            break;
        case DocumentMetadataFields::kScoreDetails:
            // When using this API to set scoreDetails (likely via $setMetadata), it's required for
            // 'scoreDetails' to have a "value" field with which 'score' will be set as well. That
            // validation is done inside setScoreAndScoreDetails().
            assertType(BSONType::object);
            setScoreAndScoreDetails(val);
            break;
        case DocumentMetadataFields::kSortKey:
            tasserted(9733901,
                      "Cannot set the sort key without knowing if it is a single element key");
        case DocumentMetadataFields::kStream:
            assertType(BSONType::object);
            setStream(std::move(val));
            break;
        case DocumentMetadataFields::kChangeStreamControlEvent:
            // The value is completely ignored here. We only set the relevant bit in the bitset.
            setChangeStreamControlEvent();
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(9733902);
    }
}

void DocumentMetadataFields::setScore(double score, bool featureFlagAlreadyValidated) {
    if (featureFlagAlreadyValidated ||
        feature_flags::gFeatureFlagRankFusionFull.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        _setCommon(MetaType::kScore);
        _holder->score = score;
    }
}

void DocumentMetadataFields::setScoreDetails(Value scoreDetails, bool featureFlagAlreadyValidated) {
    if (featureFlagAlreadyValidated ||
        feature_flags::gFeatureFlagRankFusionFull.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        _setCommon(MetaType::kScoreDetails);
        _holder->scoreDetails = scoreDetails;
    }
}

void DocumentMetadataFields::setScoreAndScoreDetails(Value scoreDetails) {
    if (feature_flags::gFeatureFlagRankFusionFull.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        auto score = scoreDetails.getDocument().getField(StringData{scoreDetailsScoreField});
        tassert(9679300,
                str::stream() << "scoreDetails must provide a numeric 'value' field with which to "
                                 "set the score too, but got "
                              << scoreDetails.toString(),
                score.numeric());

        const bool featureFlagAlreadyValidated = true;
        setScore(score.getDouble(), featureFlagAlreadyValidated);
        setScoreDetails(std::move(scoreDetails), featureFlagAlreadyValidated);
    }
}

void DocumentMetadataFields::mergeWith(const DocumentMetadataFields& other) {
    if (!hasTextScore() && other.hasTextScore()) {
        setTextScore(other.getTextScore());
    }
    if (!hasRandVal() && other.hasRandVal()) {
        setRandVal(other.getRandVal());
    }
    if (!hasSortKey() && other.hasSortKey()) {
        setSortKey(other.getSortKey(), other.isSingleElementKey());
    }
    if (!hasGeoNearDistance() && other.hasGeoNearDistance()) {
        setGeoNearDistance(other.getGeoNearDistance());
    }
    if (!hasGeoNearPoint() && other.hasGeoNearPoint()) {
        setGeoNearPoint(other.getGeoNearPoint());
    }
    if (!hasSearchScore() && other.hasSearchScore()) {
        setSearchScore(other.getSearchScore());
    }
    if (!hasSearchHighlights() && other.hasSearchHighlights()) {
        setSearchHighlights(other.getSearchHighlights());
    }
    if (!hasIndexKey() && other.hasIndexKey()) {
        setIndexKey(other.getIndexKey());
    }
    if (!hasSearchScoreDetails() && other.hasSearchScoreDetails()) {
        setSearchScoreDetails(other.getSearchScoreDetails());
    }
    if (!hasSearchRootDocumentId() && other.hasSearchRootDocumentId()) {
        setSearchRootDocumentId(other.getSearchRootDocumentId());
    }
    if (!hasSearchSequenceToken() && other.hasSearchSequenceToken()) {
        setSearchSequenceToken(other.getSearchSequenceToken());
    }
    if (!hasTimeseriesBucketMinTime() && other.hasTimeseriesBucketMinTime()) {
        setTimeseriesBucketMinTime(other.getTimeseriesBucketMinTime());
    }
    if (!hasTimeseriesBucketMaxTime() && other.hasTimeseriesBucketMaxTime()) {
        setTimeseriesBucketMaxTime(other.getTimeseriesBucketMaxTime());
    }
    if (!hasSearchSortValues() && other.hasSearchSortValues()) {
        setSearchSortValues(other.getSearchSortValues());
    }
    if (!hasVectorSearchScore() && other.hasVectorSearchScore()) {
        setVectorSearchScore(other.getVectorSearchScore());
    }
    if (!hasScore() && other.hasScore()) {
        setScore(other.getScore());
    }
    if (!hasScoreDetails() && other.hasScoreDetails()) {
        setScoreDetails(other.getScoreDetails());
    }
    if (!hasStream() && other.hasStream()) {
        setStream(other.getStream());
    }
    if (!isChangeStreamControlEvent() && other.isChangeStreamControlEvent()) {
        setChangeStreamControlEvent();
    }
}

void DocumentMetadataFields::copyFrom(const DocumentMetadataFields& other) {
    if (other.hasTextScore()) {
        setTextScore(other.getTextScore());
    }
    if (other.hasRandVal()) {
        setRandVal(other.getRandVal());
    }
    if (other.hasSortKey()) {
        setSortKey(other.getSortKey(), other.isSingleElementKey());
    }
    if (other.hasGeoNearDistance()) {
        setGeoNearDistance(other.getGeoNearDistance());
    }
    if (other.hasGeoNearPoint()) {
        setGeoNearPoint(other.getGeoNearPoint());
    }
    if (other.hasSearchScore()) {
        setSearchScore(other.getSearchScore());
    }
    if (other.hasSearchHighlights()) {
        setSearchHighlights(other.getSearchHighlights());
    }
    if (other.hasIndexKey()) {
        setIndexKey(other.getIndexKey());
    }
    if (other.hasSearchScoreDetails()) {
        setSearchScoreDetails(other.getSearchScoreDetails());
    }
    if (other.hasSearchRootDocumentId()) {
        setSearchRootDocumentId(other.getSearchRootDocumentId());
    }
    if (other.hasSearchSequenceToken()) {
        setSearchSequenceToken(other.getSearchSequenceToken());
    }
    if (other.hasTimeseriesBucketMinTime()) {
        setTimeseriesBucketMinTime(other.getTimeseriesBucketMinTime());
    }
    if (other.hasTimeseriesBucketMaxTime()) {
        setTimeseriesBucketMaxTime(other.getTimeseriesBucketMaxTime());
    }
    if (other.hasSearchSortValues()) {
        setSearchSortValues(other.getSearchSortValues());
    }
    if (other.hasVectorSearchScore()) {
        setVectorSearchScore(other.getVectorSearchScore());
    }
    if (other.hasScore()) {
        setScore(other.getScore());
    }
    if (other.hasScoreDetails()) {
        setScoreDetails(other.getScoreDetails());
    }
    if (other.hasStream()) {
        setStream(other.getStream());
    }
    if (other.isChangeStreamControlEvent()) {
        setChangeStreamControlEvent();
    }
}

size_t DocumentMetadataFields::getApproximateSize() const {
    if (!_holder) {
        return 0;
    }

    // Purposefully exclude the size of the DocumentMetadataFields, as this is accounted for
    // elsewhere. Here we only consider the "deep" size of the MetadataHolder.
    size_t size = sizeof(MetadataHolder);

    // Count the "deep" portion of the metadata values.
    size += _holder->sortKey.getApproximateSize();
    size += _holder->geoNearPoint.getApproximateSize();
    // Size of Value is double counted - once in sizeof(MetadataFields) and once in
    // getApproximateSize()
    size -= sizeof(_holder->geoNearPoint);
    size += _holder->searchHighlights.getApproximateSize();
    size -= sizeof(_holder->searchHighlights);
    size += _holder->indexKey.objsize();
    size += _holder->searchScoreDetails.objsize();
    size += _holder->searchSortValues.objsize();
    size -= sizeof(_holder->searchSequenceToken);
    size += _holder->scoreDetails.getApproximateSize();
    size -= sizeof(_holder->scoreDetails);
    size += _holder->stream.getApproximateSize();
    size -= sizeof(_holder->stream);
    return size;
}

void DocumentMetadataFields::serializeForSorter(BufBuilder& buf) const {
    // If there is no metadata, all we need to do is write a zero byte.
    if (!_holder) {
        buf.appendNum(static_cast<char>(0));
        return;
    }

    if (hasTextScore()) {
        buf.appendNum(static_cast<char>(MetaType::kTextScore + 1));
        buf.appendNum(getTextScore());
    }
    if (hasRandVal()) {
        buf.appendNum(static_cast<char>(MetaType::kRandVal + 1));
        buf.appendNum(getRandVal());
    }
    if (hasSortKey()) {
        buf.appendNum(static_cast<char>(MetaType::kSortKey + 1));
        buf.appendChar(isSingleElementKey() ? 1 : 0);
        getSortKey().serializeForSorter(buf);
    }
    if (hasGeoNearDistance()) {
        buf.appendNum(static_cast<char>(MetaType::kGeoNearDist + 1));
        buf.appendNum(getGeoNearDistance());
    }
    if (hasGeoNearPoint()) {
        buf.appendNum(static_cast<char>(MetaType::kGeoNearPoint + 1));
        getGeoNearPoint().serializeForSorter(buf);
    }
    if (hasSearchScore()) {
        buf.appendNum(static_cast<char>(MetaType::kSearchScore + 1));
        buf.appendNum(getSearchScore());
    }
    if (hasSearchHighlights()) {
        buf.appendNum(static_cast<char>(MetaType::kSearchHighlights + 1));
        getSearchHighlights().serializeForSorter(buf);
    }
    if (hasIndexKey()) {
        buf.appendNum(static_cast<char>(MetaType::kIndexKey + 1));
        getIndexKey().appendSelfToBufBuilder(buf);
    }
    if (hasSearchScoreDetails()) {
        buf.appendNum(static_cast<char>(MetaType::kSearchScoreDetails + 1));
        getSearchScoreDetails().appendSelfToBufBuilder(buf);
    }
    if (hasSearchRootDocumentId()) {
        buf.appendNum(static_cast<char>(MetaType::kSearchRootDocumentId + 1));
        getSearchRootDocumentId().serializeForSorter(buf);
    }
    if (hasSearchSequenceToken()) {
        buf.appendNum(static_cast<char>(MetaType::kSearchSequenceToken + 1));
        getSearchSequenceToken().serializeForSorter(buf);
    }
    if (hasTimeseriesBucketMinTime()) {
        buf.appendNum(static_cast<char>(MetaType::kTimeseriesBucketMinTime + 1));
        buf.appendNum(getTimeseriesBucketMinTime().toMillisSinceEpoch());
    }
    if (hasTimeseriesBucketMaxTime()) {
        buf.appendNum(static_cast<char>(MetaType::kTimeseriesBucketMaxTime + 1));
        buf.appendNum(getTimeseriesBucketMaxTime().toMillisSinceEpoch());
    }
    if (hasSearchSortValues()) {
        buf.appendNum(static_cast<char>(MetaType::kSearchSortValues + 1));
        getSearchSortValues().appendSelfToBufBuilder(buf);
    }
    if (hasVectorSearchScore()) {
        buf.appendNum(static_cast<char>(MetaType::kVectorSearchScore + 1));
        buf.appendNum(getVectorSearchScore());
    }
    if (hasScore()) {
        buf.appendNum(static_cast<char>(MetaType::kScore + 1));
        buf.appendNum(getScore());
    }
    if (hasScoreDetails()) {
        buf.appendNum(static_cast<char>(MetaType::kScoreDetails + 1));
        getScoreDetails().serializeForSorter(buf);
    }
    if (hasStream()) {
        buf.appendNum(static_cast<char>(MetaType::kStream + 1));
        getStream().serializeForSorter(buf);
    }
    if (isChangeStreamControlEvent()) {
        // When this metadata field is set, it is implicitly true, so we do not need to serialize
        // any further value for it.
        buf.appendNum(static_cast<char>(MetaType::kChangeStreamControlEvent + 1));
    }

    buf.appendNum(static_cast<char>(0));
}

void DocumentMetadataFields::deserializeForSorter(BufReader& buf, DocumentMetadataFields* out) {
    invariant(out);

    while (char marker = buf.read<char>()) {
        if (marker == static_cast<char>(MetaType::kTextScore) + 1) {
            out->setTextScore(buf.read<LittleEndian<double>>());
        } else if (marker == static_cast<char>(MetaType::kRandVal) + 1) {
            out->setRandVal(buf.read<LittleEndian<double>>());
        } else if (marker == static_cast<char>(MetaType::kSortKey) + 1) {
            char isSingleElementKey = buf.read<char>();
            out->setSortKey(Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()),
                            isSingleElementKey);
        } else if (marker == static_cast<char>(MetaType::kGeoNearDist) + 1) {
            out->setGeoNearDistance(buf.read<LittleEndian<double>>());
        } else if (marker == static_cast<char>(MetaType::kGeoNearPoint) + 1) {
            out->setGeoNearPoint(
                Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kSearchScore) + 1) {
            out->setSearchScore(buf.read<LittleEndian<double>>());
        } else if (marker == static_cast<char>(MetaType::kSearchHighlights) + 1) {
            out->setSearchHighlights(
                Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kIndexKey) + 1) {
            out->setIndexKey(
                BSONObj::deserializeForSorter(buf, BSONObj::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kSearchScoreDetails) + 1) {
            out->setSearchScoreDetails(
                BSONObj::deserializeForSorter(buf, BSONObj::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kSearchRootDocumentId) + 1) {
            out->setSearchRootDocumentId(
                Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kTimeseriesBucketMinTime) + 1) {
            out->setTimeseriesBucketMinTime(
                Date_t::fromMillisSinceEpoch(buf.read<LittleEndian<long long>>()));
        } else if (marker == static_cast<char>(MetaType::kTimeseriesBucketMaxTime) + 1) {
            out->setTimeseriesBucketMaxTime(
                Date_t::fromMillisSinceEpoch(buf.read<LittleEndian<long long>>()));
        } else if (marker == static_cast<char>(MetaType::kSearchSortValues) + 1) {
            out->setSearchSortValues(
                BSONObj::deserializeForSorter(buf, BSONObj::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kVectorSearchScore) + 1) {
            out->setVectorSearchScore(buf.read<LittleEndian<double>>());
        } else if (marker == static_cast<char>(MetaType::kSearchSequenceToken) + 1) {
            out->setSearchSequenceToken(
                Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kScore) + 1) {
            out->setScore(buf.read<LittleEndian<double>>());
        } else if (marker == static_cast<char>(MetaType::kScoreDetails) + 1) {
            out->setScoreDetails(
                Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kStream) + 1) {
            out->setStream(Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kChangeStreamControlEvent) + 1) {
            // When this metadata field is set, it is implicitly true, so it is not followed by any
            // further serialized value.
            out->setChangeStreamControlEvent();
        } else {
            uasserted(28744, "Unrecognized marker, unable to deserialize buffer");
        }
    }
}

BSONArray DocumentMetadataFields::serializeSortKey(bool isSingleElementKey, const Value& value) {
    // Missing values don't serialize correctly in this format, so use nulls instead, since they are
    // considered equivalent with woCompare().
    if (isSingleElementKey) {
        return BSON_ARRAY(missingToNull(value));
    }
    invariant(value.isArray());
    BSONArrayBuilder bb;
    for (auto&& val : value.getArray()) {
        bb << missingToNull(val);
    }
    return bb.arr();
}

Value DocumentMetadataFields::deserializeSortKey(bool isSingleElementKey,
                                                 const BSONObj& bsonSortKey) {
    std::vector<Value> keys;
    for (auto&& elt : bsonSortKey) {
        keys.push_back(Value{elt});
    }
    if (isSingleElementKey) {
        // As a special case for a sort on a single field, we do not put the keys into an array.
        return keys[0];
    }
    return Value{std::move(keys)};
}

const char* DocumentMetadataFields::typeNameToDebugString(DocumentMetadataFields::MetaType type) {
    switch (type) {
        case DocumentMetadataFields::kGeoNearDist:
            return "$geoNear distance";
        case DocumentMetadataFields::kGeoNearPoint:
            return "$geoNear point";
        case DocumentMetadataFields::kIndexKey:
            return "index key";
        case DocumentMetadataFields::kRandVal:
            return "rand val";
        case DocumentMetadataFields::kRecordId:
            return "record ID";
        case DocumentMetadataFields::kSearchHighlights:
            return "$search highlights";
        case DocumentMetadataFields::kSearchScore:
            return "$search score";
        case DocumentMetadataFields::kSortKey:
            return "sort key";
        case DocumentMetadataFields::kTextScore:
            return "text score";
        case DocumentMetadataFields::kSearchScoreDetails:
            return "$search score details";
        case DocumentMetadataFields::kSearchRootDocumentId:
            return "$search root document id";
        case DocumentMetadataFields::kTimeseriesBucketMinTime:
            return "timeseries bucket min time";
        case DocumentMetadataFields::kTimeseriesBucketMaxTime:
            return "timeseries bucket max time";
        case DocumentMetadataFields::kSearchSortValues:
            return "$search sort values";
        case DocumentMetadataFields::kSearchSequenceToken:
            return "$search sequence token";
        case DocumentMetadataFields::kVectorSearchScore:
            return "$vectorSearch score";
        case DocumentMetadataFields::kScore:
            return "score";
        case DocumentMetadataFields::kScoreDetails:
            return "scoreDetails";
        case DocumentMetadataFields::kStream:
            return "stream processing metadata";
        case DocumentMetadataFields::kChangeStreamControlEvent:
            return "change stream control event";
        default:
            MONGO_UNREACHABLE;
    }
}  // namespace


std::ostream& operator<<(std::ostream& stream, DocumentMetadataFields::MetaType type) {
    return stream << DocumentMetadataFields::typeNameToDebugString(type);
}

StringBuilder& operator<<(StringBuilder& stream, DocumentMetadataFields::MetaType type) {
    return stream << DocumentMetadataFields::typeNameToDebugString(type);
}
}  // namespace mongo
