/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include "mongo/util/field_set.h"
#include "mongo/util/string_listset.h"

namespace mongo::sbe {
/**
 * This class is used to represent a plan around how to pass the contents of the input object
 * to makeBsonObj().
 *
 * 'MakeObjInputPlan' consists of a list of individual field inputs ('_singleFields') and a set
 * of fields that are used by the plan ('_usedFields').
 */
class MakeObjInputPlan {
public:
    MakeObjInputPlan(std::vector<std::string> singleFields, FieldSet usedFields)
        : _singleFields(std::move(singleFields)),
          _usedFields(std::move(usedFields)),
          _fieldDict(buildFieldDict()) {}

    bool operator==(const MakeObjInputPlan& other) const {
        return _fieldDict == other._fieldDict;
    }

    size_t numSingleFields() const {
        return _singleFields.size();
    }
    const std::vector<std::string>& getSingleFields() {
        return _singleFields;
    }
    const FieldSet& getUsedFields() {
        return _usedFields;
    }
    const StringListSet& getFieldDict() const {
        return _fieldDict;
    }

    bool isSingleInputField(StringData field) const {
        const size_t n = numSingleFields();
        const size_t pos = _fieldDict.findPos(field);
        return pos < n;
    }

    bool isFieldUsed(StringData field) const {
        const size_t n = numSingleFields();
        const size_t pos = _fieldDict.findPos(field);
        const bool found = pos != StringListSet::npos;
        return pos >= n ? found == fieldsScopeIsClosed() : true;
    }

    inline FieldListScope getFieldsScope() const {
        return _usedFields.getScope();
    }
    bool fieldsScopeIsClosed() const {
        return _usedFields.getScope() == FieldListScope::kClosed;
    }
    bool fieldsScopeIsOpen() const {
        return _usedFields.getScope() == FieldListScope::kOpen;
    }

private:
    StringListSet buildFieldDict() {
        std::vector<std::string> dictFields;

        if (_usedFields.getScope() == FieldListScope::kClosed) {
            const auto& fieldList = _usedFields.getList();

            // Re-build '_usedFields' if needed so that the first N elements of '_usedFields' are
            // equal to the N fields from 'singleFields' in order.
            if (_singleFields.size() > fieldList.size() ||
                !std::equal(_singleFields.begin(), _singleFields.end(), fieldList.begin())) {
                auto newUsedFields = FieldSet::makeClosedSet(_singleFields);
                newUsedFields.setUnion(_usedFields);
                _usedFields = std::move(newUsedFields);
            }

            dictFields = _usedFields.getList();
        } else {
            // Ensure that all of the fields in '_singleFields' are considered "used".
            _usedFields.setUnion(FieldSet::makeClosedSet(_singleFields));
            const auto& fieldList = _usedFields.getList();

            // Concatenate '_singleFields' with '_usedFields.getList()' and store the result
            // into 'dictFields'.
            dictFields = _singleFields;
            dictFields.insert(dictFields.end(), fieldList.begin(), fieldList.end());
        }

        return StringListSet(std::move(dictFields));
    }

    std::vector<std::string> _singleFields;
    FieldSet _usedFields;
    const StringListSet _fieldDict;
};
}  // namespace mongo::sbe
