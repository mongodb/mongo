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

#include "mongo/db/query/sort_pattern.h"

namespace mongo {
SortPattern::SortPattern(const BSONObj& obj,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    for (auto&& keyField : obj) {
        auto fieldName = keyField.fieldNameStringData();

        SortPatternPart patternPart;

        if (keyField.type() == Object) {
            BSONObj metaDoc = keyField.Obj();
            // this restriction is due to needing to figure out sort direction
            uassert(17312,
                    "$meta is the only expression supported by $sort right now",
                    metaDoc.firstElement().fieldNameStringData() == "$meta");

            uassert(ErrorCodes::FailedToParse,
                    "Cannot have additional keys in a $meta sort specification",
                    metaDoc.nFields() == 1);

            VariablesParseState vps = pExpCtx->variablesParseState;
            patternPart.expression = ExpressionMeta::parse(pExpCtx, metaDoc.firstElement(), vps);

            // If sorting by textScore, sort highest scores first. If sorting by randVal, order
            // doesn't matter, so just always use descending.
            patternPart.isAscending = false;

            _sortPattern.push_back(std::move(patternPart));
            continue;
        }

        uassert(15974,
                "$sort key ordering must be specified using a number or {$meta: 'textScore'}",
                keyField.isNumber());

        int sortOrder = keyField.numberInt();

        uassert(15975,
                "$sort key ordering must be 1 (for ascending) or -1 (for descending)",
                ((sortOrder == 1) || (sortOrder == -1)));

        patternPart.fieldPath = FieldPath{fieldName};
        patternPart.isAscending = (sortOrder > 0);
        _paths.insert(patternPart.fieldPath->fullPath());
        _sortPattern.push_back(std::move(patternPart));
    }
}

Document SortPattern::serialize(SortKeySerialization serializationMode) const {
    MutableDocument keyObj;
    const size_t n = _sortPattern.size();
    for (size_t i = 0; i < n; ++i) {
        if (_sortPattern[i].fieldPath) {
            // Append a named integer based on whether the sort is ascending/descending.
            keyObj.setField(_sortPattern[i].fieldPath->fullPath(),
                            Value(_sortPattern[i].isAscending ? 1 : -1));
        } else {
            // Sorting by an expression, use a made up field name.
            auto computedFieldName = std::string(str::stream() << "$computed" << i);
            switch (serializationMode) {
                case SortKeySerialization::kForExplain:
                case SortKeySerialization::kForPipelineSerialization: {
                    const bool isExplain = (serializationMode == SortKeySerialization::kForExplain);
                    keyObj[computedFieldName] = _sortPattern[i].expression->serialize(isExplain);
                    break;
                }
                case SortKeySerialization::kForSortKeyMerging: {
                    // We need to be able to tell which direction the sort is. Expression sorts are
                    // always descending.
                    keyObj[computedFieldName] = Value(-1);
                    break;
                }
            }
        }
    }
    return keyObj.freeze();
}
}  // namespace mongo
