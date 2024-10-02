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

#include "mongo/db/query/ce/bound_utils.h"

namespace mongo::ce {

boost::optional<std::pair<sbe::value::TypeTags, sbe::value::Value>> getConstTypeVal(
    const optimizer::ABT& abt) {
    if (const auto* constant = abt.cast<optimizer::Constant>(); constant) {
        return constant->get();
    }
    return boost::none;
};

boost::optional<sbe::value::TypeTags> getConstTypeTag(const optimizer::ABT& abt) {
    if (auto maybeConstVal = getConstTypeVal(abt); maybeConstVal) {
        return maybeConstVal->first;
    }
    return boost::none;
};

}  // namespace mongo::ce
