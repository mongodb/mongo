// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/dbcheck/dbcheck_idl.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"

#include <string_view>

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
void BSONKey::serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const {
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
