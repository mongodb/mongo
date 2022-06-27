/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/plan_cache_size_parameter.h"

#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/util/pcre.h"

namespace mongo::plan_cache_util {

StatusWith<PlanCacheSizeUnits> parseUnitString(const std::string& strUnit) {
    if (strUnit.empty()) {
        return Status(ErrorCodes::Error{6007010}, "Unit value cannot be empty");
    }

    if (strUnit[0] == '%') {
        return PlanCacheSizeUnits::kPercent;
    } else if (strUnit[0] == 'M' || strUnit[0] == 'm') {
        return PlanCacheSizeUnits::kMB;
    } else if (strUnit[0] == 'G' || strUnit[0] == 'g') {
        return PlanCacheSizeUnits::kGB;
    }

    return Status(ErrorCodes::Error{6007011}, "Incorrect unit value");
}

StatusWith<PlanCacheSizeParameter> PlanCacheSizeParameter::parse(const std::string& str) {
    // Looks for a floating point number with followed by a unit suffix (MB, GB, %).
    static auto& re = *new pcre::Regex(R"re((?i)^\s*(\d+\.?\d*)\s*(MB|GB|%)\s*$)re");
    auto m = re.matchView(str);
    if (!m) {
        return {ErrorCodes::Error{6007012}, "Unable to parse plan cache size string"};
    }
    double size = std::stod(std::string{m[1]});
    std::string strUnit{m[2]};

    auto statusWithUnit = parseUnitString(strUnit);
    if (!statusWithUnit.isOK()) {
        return statusWithUnit.getStatus();
    }

    return PlanCacheSizeParameter{size, statusWithUnit.getValue()};
}

}  // namespace mongo::plan_cache_util
