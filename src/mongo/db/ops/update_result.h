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

#include "mongo/db/jsobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query_plan_selection_policy.h"

namespace mongo {

    struct UpdateResult {

        UpdateResult( bool existing_,
                      bool modifiers_,
                      unsigned long long numMatched_,
                      const BSONObj& upsertedObject_ )
            : existing(existing_)
            , modifiers(modifiers_)
            , numMatched(numMatched_) {

            upserted.clear();
            BSONElement id = upsertedObject_["_id"];
            if ( ! existing && numMatched == 1 && id.type() == jstOID ) {
                upserted = id.OID();
            }
        }


        // if existing objects were modified
        const bool existing;

        // was this a $ mod
        const bool modifiers;

        // how many objects touched
        const long long numMatched;

        // if something was upserted, the new _id of the object
        OID upserted;
    };

} // namespace mongo
