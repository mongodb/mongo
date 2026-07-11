// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/parse_log_component_settings.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/str.h"

#include <deque>
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {
StatusWith<int> tryCoerceVerbosity(BSONElement elem, std::string_view parentComponentDottedName) {
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
logv2::LogComponent _getComponentForShortName(std::string_view shortName) {
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
        const std::string_view shortName = elem.fieldNameStringData();
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
