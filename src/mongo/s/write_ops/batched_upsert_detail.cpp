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
