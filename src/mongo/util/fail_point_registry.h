/*
 *    Copyright (C) 2012 10gen Inc.
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
 */

/**
 * Should NOT be included by other header files.  Include only in source files.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/fail_point.h"

namespace mongo {
    /**
     * Class for storing FailPoint instances.
     */
    class FailPointRegistry {
    public:
        FailPointRegistry();

        /**
         * Adds a new fail point to this registry. Duplicate names are not allowed.
         *
         * @return the status code under these circumstances:
         *     OK - if successful.
         *     DuplicateKey - if the given name already exists in this registry.
         *     CannotMutateObject - if this registry is already frozen.
         */
        Status addFailPoint(const std::string& name, FailPoint* failPoint);

        /**
         * @return the fail point object registered. Returns NULL if it was not registered.
         */
        FailPoint* getFailPoint(const std::string& name) const;

        /**
         * Freezes this registry from being modified.
         */
        void freeze();

    private:
        bool _frozen;
        unordered_map<std::string, FailPoint*> _fpMap;
    };
}

