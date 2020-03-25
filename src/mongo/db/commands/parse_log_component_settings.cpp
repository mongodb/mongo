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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/commands/parse_log_component_settings.h"

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

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
    std::vector<BSONObjIterator> iterators;

    logv2::LogComponent parentComponent = logv2::LogComponent::kDefault;
    BSONObjIterator iter(settings);

    while (iter.moreWithEOO()) {
        BSONElement elem = iter.next();
        if (elem.eoo()) {
            if (!iterators.empty()) {
                iter = iterators.back();
                iterators.pop_back();
                parentComponent = parentComponent.parent();
            }
            continue;
        }
        if (elem.fieldNameStringData() == "verbosity") {
            if (!elem.isNumber()) {
                return StatusWith<Result>(ErrorCodes::BadValue,
                                          str::stream()
                                              << "Expected " << parentComponent.getDottedName()
                                              << ".verbosity to be a number, but found "
                                              << typeName(elem.type()));
            }
            levelsToSet.push_back((LogComponentSetting(parentComponent, elem.numberInt())));
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
            levelsToSet.push_back(LogComponentSetting(curr, elem.numberInt()));
            continue;
        }
        if (elem.type() != Object) {
            return StatusWith<Result>(
                ErrorCodes::BadValue,
                str::stream() << "Invalid type " << typeName(elem.type()) << "for component "
                              << parentComponent.getDottedName() << "." << shortName);
        }
        iterators.push_back(iter);
        parentComponent = curr;
        iter = BSONObjIterator(elem.Obj());
    }

    // Done walking settings
    return StatusWith<Result>(levelsToSet);
}

}  // namespace mongo
