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

#include "mongo/platform/basic.h"

#include "mongo/logv2/log_component.h"

#include "mongo/base/static_assert.h"
#include "mongo/util/assert_util.h"

namespace mongo::logv2 {

namespace {

struct {
    LogComponent value;
    StringData shortName;
    StringData logName;
    LogComponent parent;
} constexpr kTable[] = {
#define X_(id, val, shortName, logName, parent) \
    {LogComponent::id, shortName##_sd, logName##_sd, LogComponent::parent},
    MONGO_EXPAND_LOGV2_COMPONENT(X_)
#undef X_
};

// Children always come after parent component.
// This makes it unnecessary to compute children of each component
// when setting/clearing log severities in LogComponentSettings.
constexpr bool correctParentOrder(LogComponent id, LogComponent parent) {
    using V = LogComponent::Value;
    switch (V{id}) {
        case LogComponent::kAutomaticDetermination:
        case LogComponent::kDefault:
        case LogComponent::kNumLogComponents:
            return true;
        default:
            using I = std::underlying_type_t<V>;
            return I{id} > I{parent};
    }
}
#define X_(id, val, shortName, logName, parent) \
    MONGO_STATIC_ASSERT(correctParentOrder(LogComponent::id, LogComponent::parent));
MONGO_EXPAND_LOGV2_COMPONENT(X_)
#undef X_

}  // namespace

LogComponent LogComponent::parent() const {
    return kTable[_value].parent;
}

StringData LogComponent::toStringData() const {
    return kTable[_value].shortName;
}

StringData LogComponent::getNameForLog() const {
    return kTable[_value].logName;
}

std::string LogComponent::getShortName() const {
    return std::string{toStringData()};
}

namespace {
void _appendDottedName(LogComponent id, std::string* out) {
    if (id.parent() != LogComponent::kDefault) {
        _appendDottedName(id.parent(), out);
        out->append(".");
    }
    StringData shortName = id.toStringData();
    out->append(shortName.begin(), shortName.end());
}
}  // namespace

std::string LogComponent::getDottedName() const {
    if (*this == kDefault || *this == kNumLogComponents)
        return std::string{toStringData()};
    std::string out;
    _appendDottedName(*this, &out);
    return out;
}

std::ostream& operator<<(std::ostream& os, LogComponent component) {
    return os << component.getNameForLog();
}

}  // namespace mongo::logv2
