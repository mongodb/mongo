// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonelementvalue.h"

#include "mongo/bson/bsonobj.h"

namespace mongo {

BSONObj BSONElementValue::Obj() const {
    return BSONObj(value(), BSONObj::LargeSizeTrait{});
}

BSONArray BSONElementValue::Array() const {
    return BSONArray(Obj());
}

BSONObj BSONElementValue::CodeWScopeObj() const {
    int strSizeWNull = ConstDataView(value() + kCountBytes).read<LittleEndian<int>>();
    return _codeWScopeObj(strSizeWNull);
}

BSONObj BSONElementValue::_codeWScopeObj(int codeSizeWithNull) const {
    return BSONObj(value() + kCountBytes + kCountBytes + codeSizeWithNull);
}

}  // namespace mongo
