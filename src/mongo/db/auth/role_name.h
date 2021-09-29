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

#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>


#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * Representation of a name of a role in a MongoDB system.
 *
 * Consists of a "role name"  part and a "datbase name" part.
 */
class RoleName {
public:
    RoleName() = default;

    template <typename Role, typename DB>
    RoleName(Role role, DB db) {
        if constexpr (std::is_same_v<Role, std::string>) {
            _role = std::move(role);
        } else {
            _role = StringData(role).toString();
        }

        if constexpr (std::is_same_v<DB, std::string>) {
            _db = std::move(db);
        } else {
            _db = StringData(db).toString();
        }
    }

    // Added for IDL support
    static RoleName parseFromBSON(const BSONElement& elem);
    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;
    void serializeToBSON(BSONArrayBuilder* bob) const;
    void appendToBSON(BSONObjBuilder* sub) const;
    BSONObj toBSON() const;

    /**
     * Gets the name of the role excluding the "@dbname" component.
     */
    const std::string& getRole() const {
        return _role;
    }

    /**
     * Gets the database name part of a role name.
     */
    const std::string& getDB() const {
        return _db;
    }

    bool empty() const {
        return _role.empty() && _db.empty();
    }

    /**
     * Gets the full name of a role as a string, formatted as "role@db".
     *
     * Allowed for keys in non-persistent data structures, such as std::map.
     */
    std::string getDisplayName() const {
        if (empty()) {
            return "";
        }
        return str::stream() << _role << '@' << _db;
    }

    /**
     * Gets the full unambiguous unique name of a user as a string, formatted as "db.role"
     */
    std::string getUnambiguousName() const {
        if (empty()) {
            return "";
        }
        return str::stream() << _db << '.' << _role;
    }

    template <typename H>
    friend H AbslHashValue(H h, const RoleName& rname) {
        auto state = H::combine(std::move(h), rname._db);
        state = H::combine(std::move(state), '.');
        return H::combine(std::move(state), rname._role);
    }

private:
    std::string _role;
    std::string _db;
};

static inline bool operator==(const RoleName& lhs, const RoleName& rhs) {
    return (lhs.getRole() == rhs.getRole()) && (lhs.getDB() == rhs.getDB());
}

static inline bool operator!=(const RoleName& lhs, const RoleName& rhs) {
    return (lhs.getRole() != rhs.getRole()) || (lhs.getDB() != rhs.getDB());
}

static inline bool operator<(const RoleName& lhs, const RoleName& rhs) {
    if (lhs.getDB() == rhs.getDB()) {
        return lhs.getRole() < rhs.getRole();
    }
    return lhs.getDB() < rhs.getDB();
}

static inline std::ostream& operator<<(std::ostream& os, const RoleName& role) {
    if (!role.empty()) {
        os << role.getRole() << '@' << role.getDB();
    }
    return os;
}

static inline StringBuilder& operator<<(StringBuilder& os, const RoleName& role) {
    if (!role.empty()) {
        os << role.getRole() << '@' << role.getDB();
    }
    return os;
}

/**
 * Iterator over an unspecified container of RoleName objects.
 */
class RoleNameIterator {
public:
    class Impl {
        Impl(const Impl&) = delete;
        Impl& operator=(const Impl&) = delete;

    public:
        Impl(){};
        virtual ~Impl(){};
        static Impl* clone(Impl* orig) {
            return orig ? orig->doClone() : nullptr;
        }
        virtual bool more() const = 0;
        virtual const RoleName& get() const = 0;

        virtual const RoleName& next() = 0;

    private:
        virtual Impl* doClone() const = 0;
    };

    RoleNameIterator() : _impl(nullptr) {}
    RoleNameIterator(const RoleNameIterator& other) : _impl(Impl::clone(other._impl.get())) {}
    explicit RoleNameIterator(Impl* impl) : _impl(impl) {}

    RoleNameIterator& operator=(const RoleNameIterator& other) {
        _impl.reset(Impl::clone(other._impl.get()));
        return *this;
    }

    bool more() const {
        return _impl.get() && _impl->more();
    }
    const RoleName& get() const {
        return _impl->get();
    }

    const RoleName& next() {
        return _impl->next();
    }

    const RoleName& operator*() const {
        return get();
    }
    const RoleName* operator->() const {
        return &get();
    }

private:
    std::unique_ptr<Impl> _impl;
};

}  // namespace mongo

namespace mongo {

template <typename ContainerIterator>
class RoleNameContainerIteratorImpl : public RoleNameIterator::Impl {
    RoleNameContainerIteratorImpl(const RoleNameContainerIteratorImpl&) = delete;
    RoleNameContainerIteratorImpl& operator=(const RoleNameContainerIteratorImpl&) = delete;

public:
    RoleNameContainerIteratorImpl(const ContainerIterator& begin, const ContainerIterator& end)
        : _curr(begin), _end(end) {}
    virtual ~RoleNameContainerIteratorImpl() {}
    virtual bool more() const {
        return _curr != _end;
    }
    virtual const RoleName& next() {
        return *(_curr++);
    }
    virtual const RoleName& get() const {
        return *_curr;
    }
    virtual RoleNameIterator::Impl* doClone() const {
        return new RoleNameContainerIteratorImpl(_curr, _end);
    }

private:
    ContainerIterator _curr;
    ContainerIterator _end;
};

template <typename ContainerIterator>
RoleNameIterator makeRoleNameIterator(const ContainerIterator& begin,
                                      const ContainerIterator& end) {
    return RoleNameIterator(new RoleNameContainerIteratorImpl<ContainerIterator>(begin, end));
}

template <typename Container>
RoleNameIterator makeRoleNameIteratorForContainer(const Container& container) {
    return makeRoleNameIterator(container.begin(), container.end());
}

template <typename Container>
Container roleNameIteratorToContainer(RoleNameIterator it) {
    Container container;
    while (it.more()) {
        container.emplace_back(it.next());
    }
    return container;
}

}  // namespace mongo
