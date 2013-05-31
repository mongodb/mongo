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
 */

#include "mongo/db/range_deleter_service.h"

#include "mongo/base/init.h"
#include "mongo/db/range_deleter_db_env.h"

namespace {

    mongo::RangeDeleter* _deleter = NULL;
}

namespace mongo {

    MONGO_INITIALIZER(RangeDeleterInit)(InitializerContext* context) {
        _deleter = new RangeDeleter(new RangeDeleterDBEnv);
        return Status::OK();
    }

    RangeDeleter* getDeleter() {
        return _deleter;
    }
}
