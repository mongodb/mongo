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

#include "mongo/db/ops/field_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/field_ref.h"

namespace mongo {
namespace fieldchecker {

    Status basicIsUpdatable(const FieldRef& field) {
        StringData firstPart = field.getPart(0);
        if (firstPart.compare("_id") == 0) {
            return Status(ErrorCodes::BadValue, "updated cannot affect the _id");
        }
        return Status::OK();
    }

    Status isUpdatable(const FieldRef& field) {
        Status status = basicIsUpdatable(field);
        if (! status.isOK()) {
            return status;
        }

        StringData firstPart = field.getPart(0);
        if (firstPart[0] == '$') {
            return Status(ErrorCodes::BadValue, "field name cannot start with $");
        }

        return Status::OK();
    }

    Status isUpdatableLegacy(const FieldRef& field) {
        return basicIsUpdatable(field);
    }

    bool isPositional(const FieldRef& fieldRef, size_t* pos, size_t* count) {

        // 'count' is optional.
        size_t dummy;
        if (count == NULL) {
            count = &dummy;
        }

        *count = 0;
        size_t size = fieldRef.numParts();
        for (size_t i=0; i<size; i++) {
            StringData fieldPart = fieldRef.getPart(i);
            if ((fieldPart.size() == 1) && (fieldPart[0] == '$')) {
                if (*count == 0) *pos = i;
                (*count)++;
            }
        }
        return *count > 0;
    }

} // namespace fieldchecker
} // namespace mongo
