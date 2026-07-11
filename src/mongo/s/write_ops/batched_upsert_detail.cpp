// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/write_ops/batched_upsert_detail.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/field_parser.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::string;

const BSONField<int> BatchedUpsertDetail::index("index");
const BSONField<BSONObj> BatchedUpsertDetail::upsertedID("_id");

BatchedUpsertDetail::BatchedUpsertDetail() {
    clear();
}

BSONObj BatchedUpsertDetail::toBSON() const {
    BSONObjBuilder builder;

    if (_isIndexSet)
        builder.append(index(), _index);

    // We're using the BSONObj to store the _id value.
    if (_isUpsertedIDSet) {
        builder.appendAs(_upsertedID.firstElement(), upsertedID());
    }

    return builder.obj();
}

bool BatchedUpsertDetail::parseBSON(const BSONObj& source, string* errMsg) {
    clear();

    std::string dummy;
    if (!errMsg)
        errMsg = &dummy;

    FieldParser::FieldState fieldState;
    fieldState = FieldParser::extract(source, index, &_index, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isIndexSet = fieldState == FieldParser::FIELD_SET;

    fieldState = FieldParser::extractID(source, upsertedID, &_upsertedID, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isUpsertedIDSet = fieldState == FieldParser::FIELD_SET;

    return true;
}

void BatchedUpsertDetail::clear() {
    _index = 0;
    _isIndexSet = false;

    _upsertedID = BSONObj();
    _isUpsertedIDSet = false;
}

void BatchedUpsertDetail::cloneTo(BatchedUpsertDetail* other) const {
    other->clear();

    other->_index = _index;
    other->_isIndexSet = _isIndexSet;

    other->_upsertedID = _upsertedID;
    other->_isUpsertedIDSet = _isUpsertedIDSet;
}

void BatchedUpsertDetail::setIndex(int index) {
    _index = index;
    _isIndexSet = true;
}

void BatchedUpsertDetail::unsetIndex() {
    _isIndexSet = false;
}

bool BatchedUpsertDetail::isIndexSet() const {
    return _isIndexSet;
}

int BatchedUpsertDetail::getIndex() const {
    dassert(_isIndexSet);
    return _index;
}

void BatchedUpsertDetail::setUpsertedID(const BSONObj& upsertedID) {
    _upsertedID = upsertedID.firstElement().wrap("").getOwned();
    _isUpsertedIDSet = true;
}

void BatchedUpsertDetail::unsetUpsertedID() {
    _isUpsertedIDSet = false;
}

bool BatchedUpsertDetail::isUpsertedIDSet() const {
    return _isUpsertedIDSet;
}

const BSONObj& BatchedUpsertDetail::getUpsertedID() const {
    dassert(_isUpsertedIDSet);
    return _upsertedID;
}

}  // namespace mongo
