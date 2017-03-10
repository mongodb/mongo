/**
*    Copyright (C) 2017 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include <algorithm>
#include <iterator>
#include <memory>
#include <vector>

namespace mongo {
namespace transitional_tools_do_not_use {
template <typename T>
inline std::vector<T*> unspool_vector(const std::vector<std::unique_ptr<T>>& v) {
    std::vector<T*> result;
    result.reserve(v.size());
    std::transform(
        v.begin(), v.end(), std::back_inserter(result), [](const auto& p) { return p.get(); });
    return result;
}

template <typename T>
inline std::vector<std::unique_ptr<T>> spool_vector(const std::vector<T*>& v) noexcept {
    std::vector<std::unique_ptr<T>> result;
    result.reserve(v.size());
    std::transform(v.begin(), v.end(), std::back_inserter(result), [](const auto& p) {
        return std::unique_ptr<T>{p};
    });
    return result;
}

template <typename T>
inline std::vector<T*> leak_vector(std::vector<std::unique_ptr<T>>& v) noexcept {
    std::vector<T*> result;
    result.reserve(v.size());
    std::transform(
        v.begin(), v.end(), std::back_inserter(result), [](auto& p) { return p.release(); });
    return result;
}
}  // namespace transitional_tools_do_not_use
}  // namespace mongo
