/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <boost/static_assert.hpp>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/platform/cstdint.h"
#include "mongo/platform/hash_namespace.h"


namespace mongo {

    /**
     * Lock modes.
     *
     * Compatibility Matrix
     *                                          Granted mode
     *   ---------------.--------------------------------------------------------.
     *   Requested Mode | MODE_NONE  MODE_IS   MODE_IX  MODE_S   MODE_X  |
     *     MODE_IS      |      +        +         +        +        -    |
     *     MODE_IX      |      +        +         +        -        -    |
     *     MODE_S       |      +        +         -        +        -    |
     *     MODE_X       |      +        -         -        -        -    |
     */
    enum LockMode {
        MODE_NONE       = 0,
        MODE_IS         = 1,
        MODE_IX         = 2,
        MODE_S          = 3,
        MODE_X          = 4,

        // Counts the lock modes. Used for array size allocations, etc. Always insert new lock
        // modes above this entry.
        LockModesCount
    };


    /**
     * Return values for the locking functions of the lock manager.
     */
    enum LockResult {

        /**
         * The lock request was granted and is now on the granted list for the specified resource.
         */
        LOCK_OK,

        /**
         * The lock request was not granted because of conflict. If this value is returned, the
         * request was placed on the conflict queue of the specified resource and a call to the
         * LockGrantNotification::notify callback should be expected with the resource whose lock
         * was requested.
         */
        LOCK_WAITING,

        /**
         * The lock request waited, but timed out before it could be granted. This value is never
         * returned by the LockManager methods here, but by the Locker class, which offers
         * capability to block while waiting for locks.
         */
        LOCK_TIMEOUT,

        /**
         * The lock request was not granted because it would result in a deadlock. No changes to
         * the state of the Locker would be made if this value is returned (i.e., it will not be
         * killed due to deadlock). It is up to the caller to decide how to recover from this
         * return value - could be either release some locks and try again, or just bail with an
         * error and have some upper code handle it.
         */
        LOCK_DEADLOCK,

        /**
         * This is used as an initialiser value. Should never be returned.
         */
        LOCK_INVALID
    };


    /**
     * Hierarchy of resource types. The lock manager knows nothing about this hierarchy, it is
     * purely logical. Resources of different types will never conflict with each other. While the
     * lock manager does not know or care about ordering, the general policy is that resources are
     * acquired in the order below. For example, one might first acquire a RESOURCE_GLOBAL and then
     * the desired RESOURCE_DATABASE, both using intent modes, and finally a RESOURCE_COLLECTION
     * in exclusive mode.
     */
    enum ResourceType {
        // Special (singleton) resources
        RESOURCE_INVALID = 0,
        RESOURCE_GLOBAL,
        RESOURCE_MMAPV1_FLUSH,     // Necessary only for the MMAPv1 engine

        // Generic resources
        RESOURCE_DATABASE,
        RESOURCE_COLLECTION,
        RESOURCE_DOCUMENT,
        RESOURCE_MMAPv1_EXTENT_MANAGER, // Only for MMAPv1 engine, keyed on database name

        // Counts the rest. Always insert new resource types above this entry.
        ResourceTypesCount
    };

    // We only use 3 bits for the resource type in the ResourceId hash
    BOOST_STATIC_ASSERT(ResourceTypesCount <= 8);

    /**
     * Uniquely identifies a lockable resource.
     */
    class ResourceId {
    public:
        ResourceId() : _fullHash(0) { }
        ResourceId(ResourceType type, const StringData& ns);
        ResourceId(ResourceType type, const std::string& ns);
        ResourceId(ResourceType type, uint64_t hashId);

        operator size_t() const {
            return _fullHash;
        }

        bool operator== (const ResourceId& other) const {
            return _fullHash == other._fullHash;
        }

        ResourceType getType() const {
            return static_cast<ResourceType>(_type);
        }

        uint64_t getHashId() const {
            return _hashId;
        }

        std::string toString() const;

    private:

        /**
         * 64-bit hash of the resource
         */
        union {
            struct {
                uint64_t     _type   : 3;
                uint64_t     _hashId : 61;
            };

            uint64_t _fullHash;
        };

#ifdef _DEBUG
        // Keep the complete namespace name for debugging purposes (TODO: this will be removed once
        // we are confident in the robustness of the lock manager).
        std::string _nsCopy;
#endif
    };

#ifndef _DEBUG
    // Treat the resource ids as 64-bit integers in release mode in order to ensure we do not
    // spend too much time doing comparisons for hashing.
    BOOST_STATIC_ASSERT(sizeof(ResourceId) == sizeof(uint64_t));
#endif

} // namespace mongo


MONGO_HASH_NAMESPACE_START
    template <> struct hash<mongo::ResourceId> {
        size_t operator()(const mongo::ResourceId& resource) const {
            return resource;
        }
    };
MONGO_HASH_NAMESPACE_END
