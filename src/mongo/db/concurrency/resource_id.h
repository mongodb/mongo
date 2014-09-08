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
namespace newlm {

    /**
     * Hierarchy of resource types. The lock manager knows nothing about this hierarchy, it is
     * purely logical. I.e., acquiring a RESOURCE_GLOBAL and then a RESOURCE_DATABASE won't block
     * at the level of the lock manager.
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

        // Must bound the max resource id
        RESOURCE_LAST
    };

    // We only use 3 bits for the resource type in the ResourceId hash
    BOOST_STATIC_ASSERT(RESOURCE_LAST < 8);


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

        // Keep the complete namespace name for debugging purposes (TODO: this will be removed once
        // we are confident in the robustness of the lock manager).
        std::string _nsCopy;
    };

    // Treat the resource ids as 64-bit integers. Commented out for now - we will keep the full
    // resource content in there for debugging purposes.
    //
    // BOOST_STATIC_ASSERT(sizeof(ResourceId) == sizeof(uint64_t));

} // namespace newlm
} // namespace mongo


MONGO_HASH_NAMESPACE_START
    template <> struct hash<mongo::newlm::ResourceId> {
        size_t operator()(const mongo::newlm::ResourceId& resource) const {
            return resource;
        }
    };
MONGO_HASH_NAMESPACE_END
