// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/path_request.h"

#include "mongo/util/str.h"

#include <string_view>

namespace mongo::sbe::value {
std::string pathToString(const Path& p) {
    std::string out;
    size_t idx = 0;
    for (auto& component : p) {
        if (holds_alternative<Id>(component)) {
            out += "Id";
        } else if (holds_alternative<Get>(component)) {
            out += "Get(";
            out += get<Get>(component).field;
            out += ')';
        } else if (holds_alternative<Traverse>(component)) {
            out += "Traverse";
        }
        ++idx;

        if (idx != p.size()) {
            out.push_back('/');
        }
    }
    return out;
}

std::ostream& operator<<(std::ostream& os, const Path& path) {
    os << pathToString(path);
    return os;
};

std::string PathRequest::toString() const {
    return str::stream() << (type == kFilter ? "FilterPath" : "ProjectPath") << "("
                         << pathToString(path) << ")";
}

std::string_view PathRequest::getTopLevelField() const {
    return get<Get>(path[0]).field;
}

std::string PathRequest::getFullPath() const {
    StringBuilder sb;
    for (const auto& component : path) {
        if (holds_alternative<Get>(component)) {
            if (sb.len() != 0) {
                sb.append(".");
            }
            sb.append(get<Get>(component).field);
        }
    }
    return sb.str();
}
}  // namespace mongo::sbe::value
