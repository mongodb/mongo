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

#pragma once

#include <iostream>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"


namespace mongo {

class NamespaceString;

/**
 *  Representation of a shard identifier.
 */
class ShardId {
public:
    friend std::ostream& operator<<(std::ostream&, const ShardId&);

    ShardId() = default;

    // Note that this c-tor allows the implicit conversion from std::string
    ShardId(const std::string shardId) : _shardId(std::move(shardId)) {}

    // Implicit StringData conversion
    operator StringData();

    bool operator==(const ShardId&) const;
    bool operator!=(const ShardId&) const;
    bool operator==(const std::string&) const;
    bool operator!=(const std::string&) const;

    template <size_t N>
    bool operator==(const char (&val)[N]) const {
        return (strncmp(val, _shardId.data(), N) == 0);
    }

    template <size_t N>
    bool operator!=(const char (&val)[N]) const {
        return (strncmp(val, _shardId.data(), N) != 0);
    }

    // The operator<  is needed to do proper comparison in a std::map
    bool operator<(const ShardId&) const;

    const std::string& toString() const;

    /**
     * Returns -1, 0, or 1 if 'this' is less, equal, or greater than 'other' in
     * lexicographical order.
     */
    int compare(const ShardId& other) const;

    /**
     *  Returns true if _shardId is empty. Subject to include more validations in the future.
     */
    bool isValid() const;

    /**
     * Functor compatible with std::hash for std::unordered_{map,set}
     */
    struct Hasher {
        std::size_t operator()(const ShardId&) const;
    };

private:
    std::string _shardId;
};

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& stream,
                                         const ShardId& shardId) {
    return stream << shardId.toString();
}

}  // namespace mongo
