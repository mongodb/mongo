/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <iosfwd>
#include <string>

#include <boost/scoped_ptr.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/hash_namespace.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    /**
     * Representation of a name of a role in a MongoDB system.
     *
     * Consists of a "role name"  part and a "datbase name" part.
     */
    class RoleName {
    public:
        RoleName() : _splitPoint(0) {}
        RoleName(const StringData& role, const StringData& dbname);

        /**
         * Gets the name of the role excluding the "@dbname" component.
         */
        StringData getRole() const { return StringData(_fullName).substr(0, _splitPoint); }

        /**
         * Gets the database name part of a role name.
         */
        StringData getDB() const { return StringData(_fullName).substr(_splitPoint + 1); }

        bool empty() const { return _fullName.empty(); }

        /**
         * Gets the full name of a role as a string, formatted as "role@db".
         *
         * Allowed for keys in non-persistent data structures, such as std::map.
         */
        const std::string& getFullName() const { return _fullName; }

        /**
         * Stringifies the object, for logging/debugging.
         */
        const std::string& toString() const { return getFullName(); }

    private:
        std::string _fullName;  // The full name, stored as a string.  "role@db".
        size_t _splitPoint;  // The index of the "@" separating the role and db name parts.
    };

    static inline bool operator==(const RoleName& lhs, const RoleName& rhs) {
        return lhs.getFullName() == rhs.getFullName();
    }

    static inline bool operator!=(const RoleName& lhs, const RoleName& rhs) {
        return lhs.getFullName() != rhs.getFullName();
    }

    static inline bool operator<(const RoleName& lhs, const RoleName& rhs) {
        return lhs.getFullName() < rhs.getFullName();
    }

    std::ostream& operator<<(std::ostream& os, const RoleName& name);


    /**
     * Iterator over an unspecified container of RoleName objects.
     */
    class RoleNameIterator {
    public:
        class Impl {
            MONGO_DISALLOW_COPYING(Impl);
        public:
            Impl() {};
            virtual ~Impl() {};
            static Impl* clone(Impl* orig) { return orig ? orig->doClone(): NULL; }
            virtual bool more() const = 0;
            virtual const RoleName& get() const = 0;

            virtual const RoleName& next() = 0;

        private:
            virtual Impl* doClone() const = 0;
        };

        RoleNameIterator() : _impl(NULL) {}
        RoleNameIterator(const RoleNameIterator& other) : _impl(Impl::clone(other._impl.get())) {}
        explicit RoleNameIterator(Impl* impl) : _impl(impl) {}

        RoleNameIterator& operator=(const RoleNameIterator& other) {
            _impl.reset(Impl::clone(other._impl.get()));
            return *this;
        }

        bool more() const { return _impl.get() && _impl->more(); }
        const RoleName& get() const { return _impl->get(); }

        const RoleName& next() { return _impl->next(); }

        const RoleName& operator*() const { return get(); }
        const RoleName* operator->() const { return &get(); }

    private:
        boost::scoped_ptr<Impl> _impl;
    };

} // namespace mongo

// Define hash function for RoleNames so they can be keys in std::unordered_map
MONGO_HASH_NAMESPACE_START
    template <> struct hash<mongo::RoleName> {
        size_t operator()(const mongo::RoleName& rname) const {
            return hash<std::string>()(rname.getFullName());
        }
    };
MONGO_HASH_NAMESPACE_END

namespace mongo {

    // RoleNameIterator for iterating over an unordered_set of RoleNames.
    class RoleNameSetIterator : public RoleNameIterator::Impl {
        MONGO_DISALLOW_COPYING(RoleNameSetIterator);

    public:
        RoleNameSetIterator(const unordered_set<RoleName>::const_iterator& begin,
                            const unordered_set<RoleName>::const_iterator& end);

        virtual ~RoleNameSetIterator();

        virtual bool more() const;

        virtual const RoleName& next();

        virtual const RoleName& get() const;

    private:
        virtual Impl* doClone() const;

        unordered_set<RoleName>::const_iterator _begin;
        unordered_set<RoleName>::const_iterator _end;
    };

}  // namespace mongo
