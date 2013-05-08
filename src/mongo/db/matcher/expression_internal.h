// expression_internal.h

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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    // XXX document me
    string pathToString( const FieldRef& path, int32_t size );

    // XXX document me
    // Replaces getFieldDottedOrArray without recursion nor string manipulation
    BSONElement getFieldDottedOrArray( const BSONObj& doc,
                                       const FieldRef& path,
                                       size_t* idxPath,
                                       bool* inArray );

}  // namespace mongo
