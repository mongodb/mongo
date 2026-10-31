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
    const int valueSize = ConstDataView(value()).read<LittleEndian<int>>();
    constexpr auto minCodeWScopeSize = 8 + static_cast<int>(BSONObj::kMinBSONLength);
    tassert(ErrorCodes::BSONInternalError,
            "BSONObj scope starts outside codeWScope value",
            valueSize >= minCodeWScopeSize && codeSizeWithNull >= 1 &&
                codeSizeWithNull <= valueSize - minCodeWScopeSize);
    const char* const start = value() + kCountBytes + kCountBytes + codeSizeWithNull;
    const int scopeSize = ConstDataView(start).read<LittleEndian<int>>();
    tassert(ErrorCodes::BSONInternalError,
            "BSONObj scope extends past codeWScope value",
            scopeSize >= static_cast<int>(BSONObj::kMinBSONLength) &&
                scopeSize <= valueSize - minCodeWScopeSize);
    return BSONObj(start);
}

}  // namespace mongo
