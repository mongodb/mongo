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

#include "mongo/base/init.h"
#include "mongo/util/fail_point_registry.h"

namespace mongo {
    /**
     * @return the global fail point registry.
     */
    FailPointRegistry* getGlobalFailPointRegistry();

    /**
     * Convenience macro for declaring a fail point. Must be used in global scope and
     * never in a block with limited scope (ie, inside functions, loops, etc.)
     */
    #define MONGO_FP_DECLARE(fp) FailPoint fp; \
        MONGO_INITIALIZER_GENERAL(fp, ("FailPointRegistry"), ("AllFailPointsRegistered")) \
                (::mongo::InitializerContext* context) { \
            return getGlobalFailPointRegistry()->addFailPoint(#fp, &fp); \
        }
}
