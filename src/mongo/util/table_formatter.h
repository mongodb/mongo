/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include <algorithm>
#include <iomanip>

namespace {
const int kDefaultColumnSpacing = 3;
}

/**
 * A tool to shape data into a table. Input may be any iterable type T of another
 * iterable type U of a string-like type S, for example, a vector of vectors of
 * std::strings. Type S must support an ostream overload and a size() method.
 *
 * Example usage:
 *     std::vector<std::vector<std::string>> rows;
 *
 *     rows.push_back({ "X_VALUE", "Y_VALUE" });
 *     rows.push_back({ "0", "0" });
 *     rows.push_back({ "10.3", "0" });
 *     rows.push_back({ "-0.5", "2" });
 *
 *     std::cout << toTable(rows) << std::endl;
 */
template <typename T>
std::string toTable(const T& rows) {
    std::vector<std::size_t> widths;

    for (auto&& row : rows) {
        size_t i = 0;
        for (auto&& value : row) {
            widths.resize(std::max(widths.size(), i + 1));
            widths[i] = std::max(widths[i], value.size());
            i++;
        }
    }

    std::stringstream ss;
    ss << std::left;

    for (auto&& row : rows) {
        size_t i = 0;
        for (auto&& value : row) {
            ss << std::setw(widths[i++] + kDefaultColumnSpacing);
            ss << value;
        }
        ss << "\n";
    }

    return ss.str();
}
