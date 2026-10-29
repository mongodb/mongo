// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/runtime_update_path.h"


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
