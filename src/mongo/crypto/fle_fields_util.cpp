/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/crypto/fle_fields_util.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/basic_types_gen.h"

namespace mongo {
void validateIDLFLE2EncryptionPlaceholder(const FLE2EncryptionPlaceholder* placeholder) {
    if (placeholder->getAlgorithm() == Fle2AlgorithmInt::kRange) {
        if (placeholder->getType() == Fle2PlaceholderType::kFind) {
            auto val = placeholder->getValue().getElement();
            uassert(6720200, "Range Find placeholder value must be an object.", val.isABSONObj());
            auto obj = val.Obj();
            FLE2RangeSpec::parse(IDLParserContext("v"), obj);
            uassert(6832501,
                    "Sparsity must be defined for range placeholders.",
                    placeholder->getSparsity());
        } else if (placeholder->getType() == Fle2PlaceholderType::kInsert) {
            auto val = placeholder->getValue().getElement();
            uassert(6775321, "Range Insert placeholder value must be an object.", val.isABSONObj());
            auto obj = val.Obj();
            FLE2RangeInsertSpec::parse(IDLParserContext("v"), obj);
            uassert(6775322,
                    "Sparsity must be defined for range placeholders.",
                    placeholder->getSparsity());
        }
    } else {
        uassert(6832500,
                "Hypergraph sparsity can only be set for range placeholders.",
                !placeholder->getSparsity());
    }
}

void validateIDLFLE2RangeSpec(const FLE2RangeSpec* placeholder) {
    auto min = placeholder->getMin().getElement();
    auto max = placeholder->getMax().getElement();
    uassert(6833400,
            str::stream() << "Minimum element in a range must be numeric, not: " << min.type(),
            min.isNumber() || min.type() == BSONType::MinKey);
    uassert(6833401,
            str::stream() << "Maximum element in a range must be numeric, not: " << max.type(),
            max.isNumber() || max.type() == BSONType::MaxKey);
}
}  // namespace mongo
