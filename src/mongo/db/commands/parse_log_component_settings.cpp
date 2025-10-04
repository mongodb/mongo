/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/commands/parse_log_component_settings.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/str.h"

#include <deque>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {
StatusWith<int> tryCoerceVerbosity(BSONElement elem, StringData parentComponentDottedName) {
    int newVerbosityLevel;
    Status coercionStatus = elem.tryCoerce(&newVerbosityLevel);
    if (!coercionStatus.isOK()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Expected " << parentComponentDottedName << '.'
                              << elem.fieldNameStringData()
                              << " to be safely cast to integer, but could not: "
                              << coercionStatus.reason()};
    } else if (newVerbosityLevel < -1) {
        return {ErrorCodes::BadValue,
                str::stream() << "Expected " << parentComponentDottedName << '.'
                              << elem.fieldNameStringData()
                              << " to be greater than or equal to -1, but found "
                              << elem.toString(false, false)};
    }

    return newVerbosityLevel;
}

}  // namespace

/*
 * Looks up a component by its short name, or returns kNumLogComponents
 * if the shortName is invalid
 */
logv2::LogComponent _getComponentForShortName(StringData shortName) {
    for (int i = 0; i < int(logv2::LogComponent::kNumLogComponents); ++i) {
        logv2::LogComponent component = static_cast<logv2::LogComponent::Value>(i);
        if (component.getShortName() == shortName)
            return component;
    }
    return static_cast<logv2::LogComponent::Value>(logv2::LogComponent::kNumLogComponents);
}

StatusWith<std::vector<LogComponentSetting>> parseLogComponentSettings(const BSONObj& settings) {
    typedef std::vector<LogComponentSetting> Result;

    std::vector<LogComponentSetting> levelsToSet;

    logv2::LogComponent parentComponent = logv2::LogComponent::kDefault;
    struct Progress {
        BSONObj obj;
        BSONObjStlIterator iter = obj.begin();
    };
    std::deque<Progress> parseStack{{settings}};

    while (!parseStack.empty()) {
        auto& [obj, iter] = parseStack.back();
        if (iter == obj.end()) {
            parseStack.pop_back();
            parentComponent = parentComponent.parent();
            continue;
        }
        BSONElement elem = *iter++;
        if (elem.fieldNameStringData() == "verbosity") {
            auto swVerbosity = tryCoerceVerbosity(elem, parentComponent.getDottedName());
            if (!swVerbosity.isOK()) {
                return swVerbosity.getStatus();
            }

            levelsToSet.push_back((LogComponentSetting(parentComponent, swVerbosity.getValue())));
            continue;
        }
        const StringData shortName = elem.fieldNameStringData();
        const logv2::LogComponent curr = _getComponentForShortName(shortName);

        if (curr == logv2::LogComponent::kNumLogComponents || curr.parent() != parentComponent) {
            return StatusWith<Result>(ErrorCodes::BadValue,
                                      str::stream()
                                          << "Invalid component name "
                                          << parentComponent.getDottedName() << "." << shortName);
        }
        if (elem.isNumber()) {
            auto swVerbosity = tryCoerceVerbosity(elem, parentComponent.getDottedName());
            if (!swVerbosity.isOK()) {
                return swVerbosity.getStatus();
            }
            levelsToSet.push_back((LogComponentSetting(curr, swVerbosity.getValue())));
            continue;
        }
        if (elem.type() != BSONType::object) {
            return StatusWith<Result>(
                ErrorCodes::BadValue,
                str::stream() << "Invalid type " << typeName(elem.type()) << "for component "
                              << parentComponent.getDottedName() << "." << shortName);
        }
        parentComponent = curr;
        parseStack.push_back({elem.Obj()});
    }

    // Done walking settings
    return StatusWith<Result>(levelsToSet);
}

}  // namespace mongo
