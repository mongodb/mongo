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

#include "mongo/db/update/runtime_update_path.h"

#include "mongo/bson/util/builder.h"

namespace mongo {
void RuntimeUpdatePath::reportError() const {
    StringBuilder fieldRefInfo;
    for (FieldIndex i = 0; i < _fieldRef.numParts(); ++i) {
        auto ref = _fieldRef.getPart(i);
        if (FieldRef::isNumericPathComponentStrict(ref)) {
            fieldRefInfo << "numeric strict,";
        } else if (FieldRef::isNumericPathComponentLenient(ref)) {
            fieldRefInfo << "numeric lenient,";
        } else {
            fieldRefInfo << "not numeric,";
        }
    }
    StringBuilder typeInfo;
    for (auto type : _types) {
        switch (type) {
            case kFieldName:
                typeInfo << "field name,";
                break;
            case kArrayIndex:
                typeInfo << "array index";
                break;
        }
    }
    tasserted(9123700,
              fmt::format("FieldRef and type vector size not matched. FieldRef size: {}, "
                          "type size: {}, _types is [{}], ref is [{}]",
                          _fieldRef.numParts(),
                          _types.size(),
                          typeInfo.str(),
                          fieldRefInfo.str()));
}
}  // namespace mongo
