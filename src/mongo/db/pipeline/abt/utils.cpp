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

#include "mongo/db/pipeline/abt/utils.h"

namespace mongo::optimizer {

template <bool isConjunction, typename... Args>
ABT generateConjunctionOrDisjunction(Args&... args) {
    ABTVector elements;
    (elements.emplace_back(args), ...);

    if (elements.size() == 0) {
        return Constant::boolean(isConjunction);
    }

    ABT result = std::move(elements.at(0));
    for (size_t i = 1; i < elements.size(); i++) {
        result = make<BinaryOp>(isConjunction ? Operations::And : Operations::Or,
                                std::move(elements.at(i)),
                                std::move(result));
    }
    return result;
}

std::pair<sbe::value::TypeTags, sbe::value::Value> convertFrom(const Value val) {
    // TODO: Either make this conversion unnecessary by changing the value representation in
    // ExpressionConstant, or provide a nicer way to convert directly from Document/Value to
    // sbe::Value.
    BSONObjBuilder bob;
    val.addToBsonObj(&bob, ""_sd);
    auto obj = bob.done();
    auto be = obj.objdata();
    auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
    return sbe::bson::convertFrom<false>(be + 4, end, 0);
}

ABT translateFieldPath(const FieldPath& fieldPath,
                       ABT initial,
                       const ABTFieldNameFn& fieldNameFn,
                       const size_t skipFromStart) {
    ABT result = std::move(initial);

    const size_t fieldPathLength = fieldPath.getPathLength();
    bool isLastElement = true;
    for (size_t i = fieldPathLength; i-- > skipFromStart;) {
        result =
            fieldNameFn(fieldPath.getFieldName(i).toString(), isLastElement, std::move(result));
        isLastElement = false;
    }

    return result;
}

}  // namespace mongo::optimizer
