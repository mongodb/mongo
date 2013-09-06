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

#include "mongo/db/index_selection.h"

#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/queryutil.h"

namespace mongo {

    IndexSuitability IndexSelection::isSuitableFor(const BSONObj& keyPattern,
                                                   const FieldRangeSet& queryConstraints,
                                                   const BSONObj& order) {

        string type = CatalogHack::getAccessMethodName(keyPattern);
        BSONObj query = queryConstraints.originalQuery();

        // "" means it's a b-tree index, ascending or descending.
        if ("" == type) {
            // This is a quick first pass to determine the suitability of the index.  It produces
            // some false positives (returns HELPFUL for some indexes which are not particularly).
            // When we return HELPFUL a more precise determination of utility is done by the query
            // optimizer.

            // check whether any field in the index is constrained at all by the query
            BSONForEach( elt, keyPattern ){
                const FieldRange& frange = queryConstraints.range( elt.fieldName() );
                if( ! frange.universal() )
                    return HELPFUL;
            }

            // or whether any field in the desired sort order is in the index
            set<string> orderFields;
            order.getFieldNames( orderFields );
            BSONForEach( k, keyPattern ) {
                if ( orderFields.find( k.fieldName() ) != orderFields.end() )
                    return HELPFUL;
            }
            return USELESS;
        } else if (IndexNames::GEO_2D == type) {
            string fieldName;

            BSONObjIterator i(keyPattern);
            while (i.more()) {
                BSONElement ie = i.next();

                if (ie.type() == String && IndexNames::GEO_2D == ie.valuestr()) {
                    fieldName = ie.fieldName();
                    break;
                }
            }

            verify("" != fieldName);

            BSONElement e = query.getFieldDotted(fieldName);
            switch (e.type()) {
            case Object: {
                BSONObj sub = e.embeddedObject();
                switch (sub.firstElement().getGtLtOp()) {
                case BSONObj::opNEAR:
                    return OPTIMAL;
                case BSONObj::opWITHIN: {
                    // Don't return optimal if it's $within: {$geometry: ... }
                    // because we will error out in that case, but the matcher
                    // or 2dsphere index may handle it.
                    BSONElement elt = sub.firstElement();
                    if (Object == elt.type()) {
                        BSONObjIterator it(elt.embeddedObject());
                        while (it.more()) {
                            BSONElement elt = it.next();
                            if (mongoutils::str::equals("$geometry", elt.fieldName())) {
                                return USELESS;
                            }
                        }
                    }
                    return OPTIMAL;
                }
                default:
                    // We can try to match if there's no other indexing defined,
                    // this is assumed a point
                    return HELPFUL;
                }
            }
            case Array:
                // We can try to match if there's no other indexing defined,
                // this is assumed a point
                return HELPFUL;
            default:
                return USELESS;
            }
        } else if (IndexNames::HASHED == type) {
            /* This index is only considered "HELPFUL" for a query
             * if it's the union of at least one equality constraint on the
             * hashed field.  Otherwise it's considered USELESS.
             * Example queries (supposing the indexKey is {a : "hashed"}):
             *   {a : 3}  HELPFUL
             *   {a : 3 , b : 3} HELPFUL
             *   {a : {$in : [3,4]}} HELPFUL
             *   {a : {$gte : 3, $lte : 3}} HELPFUL
             *   {} USELESS
             *   {b : 3} USELESS
             *   {a : {$gt : 3}} USELESS
             */
            BSONElement firstElt = keyPattern.firstElement();
            if (queryConstraints.isPointIntervalSet(firstElt.fieldName())) {
                return HELPFUL;
            } else {
                return USELESS;
            }
        } else if (IndexNames::GEO_2DSPHERE == type) {
            BSONObjIterator i(keyPattern);
            while (i.more()) {
                BSONElement ie = i.next();

                if (ie.type() != String || IndexNames::GEO_2DSPHERE != ie.valuestr()) {
                    continue;
                }

                BSONElement e = query.getFieldDotted(ie.fieldName());
                // Some locations are given to us as arrays.  Sigh.
                if (Array == e.type()) { return HELPFUL; }
                if (Object != e.type()) { continue; }
                // getGtLtOp is horribly misnamed and really means get the operation.
                switch (e.embeddedObject().firstElement().getGtLtOp()) {
                    case BSONObj::opNEAR:
                        return OPTIMAL;
                    case BSONObj::opWITHIN: {
                        BSONElement elt = e.embeddedObject().firstElement();
                        if (Object != elt.type()) { continue; }
                        const char* fname = elt.embeddedObject().firstElement().fieldName();
                        if (mongoutils::str::equals("$geometry", fname)
                            || mongoutils::str::equals("$centerSphere", fname)) {
                            return OPTIMAL;
                        } else {
                            return USELESS;
                        }
                    }
                    case BSONObj::opGEO_INTERSECTS:
                        return OPTIMAL;
                    default:
                        return USELESS;
                }
            }
            return USELESS;
        } else if (IndexNames::TEXT == type || IndexNames::TEXT_INTERNAL == type) {
            return USELESS;
        } else if (IndexNames::GEO_HAYSTACK == type) {
            return USELESS;
        } else {
            cout << "Can't find index for keypattern " << keyPattern << endl;
            verify(0);
            return USELESS;
        }
    }

}  // namespace mongo
