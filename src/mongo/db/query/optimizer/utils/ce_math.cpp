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

#include <algorithm>   // std::sort
#include <cmath>       // std::pow
#include <functional>  // std::greater

#include "mongo/db/query/optimizer/utils/ce_math.h"

namespace mongo::ce {

bool validSelectivity(SelectivityType sel) {
    return (sel >= 0.0 && sel <= 1.0);
}

bool validCardinality(CEType card) {
    return (card >= kMinCard && card <= std::numeric_limits<CEType>::max());
}

SelectivityType conjExponentialBackoff(std::vector<SelectivityType> conjSelectivities) {
    size_t actualMaxBackoffElements = std::min(conjSelectivities.size(), kMaxBackoffElements);
    std::partial_sort(conjSelectivities.begin(),
                      conjSelectivities.begin() + actualMaxBackoffElements,
                      conjSelectivities.end());
    SelectivityType sel = conjSelectivities[0];
    SelectivityType f = 1.0;
    size_t i = 1;
    while (i < actualMaxBackoffElements) {
        f /= 2.0;
        sel *= std::pow(conjSelectivities[i], f);
        i++;
    }
    return sel;
}

SelectivityType disjExponentialBackoff(std::vector<SelectivityType> disjSelectivities) {
    size_t actualMaxBackoffElements = std::min(disjSelectivities.size(), kMaxBackoffElements);
    std::partial_sort(disjSelectivities.begin(),
                      disjSelectivities.begin() + actualMaxBackoffElements,
                      disjSelectivities.end(),
                      std::greater<SelectivityType>());
    SelectivityType sel = 1.0 - disjSelectivities[0];
    SelectivityType f = 1.0;
    size_t i = 1;
    while (i < actualMaxBackoffElements) {
        f /= 2.0;
        sel *= std::pow(1 - disjSelectivities[i], f);
        i++;
    }
    return 1.0 - sel;
}
}  // namespace mongo::ce
