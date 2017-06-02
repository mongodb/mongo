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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/**
 * A KVPrefix may be prepended to the keys of entries in an underlying KV store. Prefixing keys as
 * such allows multiple MongoDB collections share an underlying table. This can be a beneficial
 * tradeoff for workloads that create many collections.
 */
class KVPrefix {
public:
    // Represents a table that is not grouped and should not have its keys prefixed.
    static const KVPrefix kNotPrefixed;

    bool isPrefixed() const {
        return _value >= 0;
    }

    int64_t toBSONValue() const {
        return _value;
    }

    int64_t repr() const {
        return _value;
    }

    std::string toString() const;

    inline bool operator<(const KVPrefix& rhs) const {
        return _value < rhs._value;
    }

    inline bool operator==(const KVPrefix& rhs) const {
        return _value == rhs._value;
    }

    inline bool operator!=(const KVPrefix& rhs) const {
        return _value != rhs._value;
    }

    static KVPrefix fromBSONElement(const BSONElement value);

    static void setLargestPrefix(KVPrefix largestPrefix);

    /**
     * Returns 'KVPrefix::kNotPrefixed' if 'storageGlobalParams.groupCollections' is false or the
     * input 'ns' is a namespace disallowed for grouping. Otherwise returns the next 'KVPrefix'
     * ensuring it is unique with respect to active collections and indexes.
     */
    static KVPrefix getNextPrefix(const NamespaceString& ns);

    /**
     * Unconditionally returns a new prefix. Only useful for testing.
     */
    static KVPrefix generateNextPrefix();

private:
    explicit KVPrefix(int64_t value) : _value(value) {}
    int64_t _value;

    static stdx::mutex _nextValueMutex;
    static int64_t _nextValue;
};

inline std::ostream& operator<<(std::ostream& s, const KVPrefix& prefix) {
    return (s << prefix.toString());
}
}
