// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/log_component.h"

#include "mongo/base/static_assert.h"

#include <ostream>
#include <string_view>

namespace mongo::logv2 {

namespace {

using namespace std::string_view_literals;

struct {
    LogComponent value;
    std::string_view shortName;
    std::string_view logName;
    LogComponent parent;
} constexpr kTable[] = {
#define X_(id, val, shortName, logName, parent) \
    {LogComponent::id, shortName ""sv, logName ""sv, LogComponent::parent},
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

std::string_view LogComponent::toStringData() const {
    return kTable[_value].shortName;
}

std::string_view LogComponent::getNameForLog() const {
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
    std::string_view shortName = id.toStringData();
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
