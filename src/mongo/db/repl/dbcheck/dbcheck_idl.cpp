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

#include "mongo/db/repl/dbcheck/dbcheck_idl.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"

namespace mongo {

BSONKey BSONKey::parseFromBSON(const BSONElement& element) {
    return BSONKey(element);
}

BSONKey BSONKey::min() {
    BSONKey result;
    result._obj = BSON("_id" << MINKEY);
    return result;
}

BSONKey BSONKey::max() {
    BSONKey result;
    result._obj = BSON("_id" << MAXKEY);
    return result;
}

/**
 * Serialize this class as a field in a document.
 */
void BSONKey::serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
    builder->appendAs(_obj.firstElement(), fieldName);
}

const BSONObj& BSONKey::obj() const {
    return _obj;
}

BSONElement BSONKey::elem() const {
    return _obj["_id"];
}

BSONKey::BSONKey(const BSONElement& elem) {
    BSONObjBuilder builder;
    builder.appendAs(elem, "_id");
    _obj = builder.obj();
}

bool BSONKey::operator==(const BSONKey& other) const {
    return SimpleBSONObjComparator::kInstance.evaluate(_obj == other._obj);
}

bool BSONKey::operator!=(const BSONKey& other) const {
    return SimpleBSONObjComparator::kInstance.evaluate(_obj != other._obj);
}

bool BSONKey::operator<(const BSONKey& other) const {
    return SimpleBSONObjComparator::kInstance.evaluate(_obj < other._obj);
}

bool BSONKey::operator<=(const BSONKey& other) const {
    return SimpleBSONObjComparator::kInstance.evaluate(_obj <= other._obj);
}

bool BSONKey::operator>(const BSONKey& other) const {
    return SimpleBSONObjComparator::kInstance.evaluate(_obj > other._obj);
}

bool BSONKey::operator>=(const BSONKey& other) const {
    return SimpleBSONObjComparator::kInstance.evaluate(_obj >= other._obj);
}
bool BSONKey::operator==(const BSONElement& other) const {
    return elem().woCompare(other, false) == 0;
}

bool BSONKey::operator!=(const BSONElement& other) const {
    return elem().woCompare(other, false) != 0;
}

bool BSONKey::operator<(const BSONElement& other) const {
    return elem().woCompare(other, false) < 0;
}

bool BSONKey::operator<=(const BSONElement& other) const {
    return elem().woCompare(other, false) <= 0;
}

bool BSONKey::operator>(const BSONElement& other) const {
    return elem().woCompare(other, false) > 0;
}

bool BSONKey::operator>=(const BSONElement& other) const {
    return elem().woCompare(other, false) >= 0;
}

}  // namespace mongo
