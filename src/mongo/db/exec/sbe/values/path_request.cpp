/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/path_request.h"

#include "mongo/db/exec/sbe/values/value.h"

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

StringData PathRequest::getTopLevelField() const {
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
