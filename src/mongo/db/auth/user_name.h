/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <iosfwd>
#include <string>

#include "mongo/base/string_data.h"

namespace mongo {

    /**
     * Representation of a name of a principal (authenticatable user) in a MongoDB system.
     *
     * Consists of a "user name" part, and a "database name" part.
     */
    class UserName {
    public:
        UserName() : _splitPoint(0) {}
        UserName(const StringData& user, const StringData& dbname);

        /**
         * Gets the user part of a UserName.
         */
        StringData getUser() const { return StringData(_fullName).substr(0, _splitPoint); }

        /**
         * Gets the database name part of a UserName.
         */
        StringData getDB() const { return StringData(_fullName).substr(_splitPoint + 1); }

        /**
         * Gets the full unique name of a user as a string, formatted as "user@db".
         */
        const std::string& getFullName() const { return _fullName; }

        /**
         * Stringifies the object, for logging/debugging.
         */
        std::string toString() const { return getFullName(); }

    private:
        std::string _fullName;  // The full name, stored as a string.  "user@db".
        size_t _splitPoint;  // The index of the "@" separating the user and db name parts.
    };

    static inline bool operator==(const UserName& lhs, const UserName& rhs) {
        return lhs.getFullName() == rhs.getFullName();
    }

    static inline bool operator!=(const UserName& lhs, const UserName& rhs) {
        return lhs.getFullName() != rhs.getFullName();
    }

    static inline bool operator<(const UserName& lhs, const UserName& rhs) {
        return lhs.getFullName() < rhs.getFullName();
    }

    std::ostream& operator<<(std::ostream& os, const UserName& name);

}  // namespace mongo
