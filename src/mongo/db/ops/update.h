//@file update.h

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/query_plan_selection_policy.h"

namespace mongo {

    class UpdateDriver;

    UpdateResult update(UpdateRequest& request);

    UpdateResult update(UpdateRequest& request, UpdateDriver* driver);

    /**
     * takes the from document and returns a new document
     * after apply all the operators
     * e.g.
     *   applyUpdateOperators( BSON( "x" << 1 ) , BSON( "$inc" << BSON( "x" << 1 ) ) );
     *   returns: { x : 2 }
     */
    BSONObj applyUpdateOperators( const BSONObj& from, const BSONObj& operators );
}  // namespace mongo
