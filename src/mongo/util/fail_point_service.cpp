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

#include "mongo/util/fail_point_service.h"

namespace mongo {
    MONGO_FP_DECLARE(dummy); // used by jstests/libs/fail_point.js

    scoped_ptr<FailPointRegistry> _fpRegistry(NULL);

    MONGO_INITIALIZER(FailPointRegistry)(InitializerContext* context) {
        _fpRegistry.reset(new FailPointRegistry());
        return Status::OK();
    }

    MONGO_INITIALIZER_GENERAL(AllFailPointsRegistered, (), ())(InitializerContext* context) {
        _fpRegistry->freeze();
        return Status::OK();
    }

    FailPointRegistry* getGlobalFailPointRegistry() {
        return _fpRegistry.get();
    }
}
