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

#include "mongo/db/query/stage_builder/sbe/builder_data.h"

namespace mongo::stage_builder {

std::string PlanStageData::debugString(boost::optional<size_t> lengthCap /*= boost::none*/) const {
    StringBuilder builder;
    StringBuilder tmp;

    if (auto slot = staticData->resultSlot) {
        tmp << "$$RESULT=s" << *slot << " ";
        if (lengthCap.has_value() && static_cast<size_t>(builder.len() + tmp.len()) > lengthCap) {
            builder << "...";
            return builder.str();
        }
        builder << tmp.str();
        tmp.reset();
    }
    if (auto slot = staticData->recordIdSlot) {
        builder << "$$RID=s" << *slot << " ";
        if (static_cast<size_t>(builder.len() + tmp.len()) > lengthCap) {
            builder << "...";
            return builder.str();
        }
        builder << tmp.str();
        tmp.reset();
    }

    if (lengthCap.has_value() && static_cast<size_t>(builder.len()) >= lengthCap.get()) {
        builder << "...";
        return builder.str();
    }
    boost::optional<size_t> curLengthCap = lengthCap.has_value()
        ? boost::make_optional(lengthCap.get() - static_cast<size_t>(builder.len()))
        : boost::none;
    env->debugString(&builder, curLengthCap);

    return builder.str();
}


}  // namespace mongo::stage_builder
