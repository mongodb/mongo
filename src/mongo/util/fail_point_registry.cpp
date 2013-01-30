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

#include "mongo/util/fail_point_registry.h"

#include "mongo/db/commands.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"

using mongoutils::str::stream;

namespace mongo {
    FailPointRegistry::FailPointRegistry(): _frozen(false) {
    }

    Status FailPointRegistry::addFailPoint(const string& name,
            FailPoint* failPoint) {
        if (_frozen) {
            return Status(ErrorCodes::CannotMutateObject, "Registry is already frozen");
        }

        if (_fpMap.count(name) > 0) {
            return Status(ErrorCodes::DuplicateKey,
                    stream() << "Fail point already registered: " << name);
        }

        _fpMap.insert(make_pair(name, failPoint));
        return Status::OK();
    }

    FailPoint* FailPointRegistry::getFailPoint(const string& name) const {
        return mapFindWithDefault(_fpMap, name, reinterpret_cast<FailPoint *>(NULL));
    }

    void FailPointRegistry::freeze() {
        _frozen = true;
    }
}
