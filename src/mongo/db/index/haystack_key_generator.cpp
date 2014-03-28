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

#include "mongo/db/index/haystack_access_method.h"

#include "mongo/base/status.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/haystack_access_method_internal.h"
#include "mongo/db/index/haystack_key_generator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/internal_plans.h"

namespace mongo {

    // -------------------

    HaystackKeyGenerator::HaystackKeyGenerator( const std::string& geoField,
                                                const std::vector<std::string>& otherFields,
                                                double bucketSize )
        : _geoField( geoField ),
          _otherFields( otherFields ),
          _bucketSize( bucketSize ) {
    }

    namespace {
        /**
         * Build a new BSONObj with root in it.  If e is non-empty, append that to the key.
         * Insert the BSONObj into keys.
         * Used by getHaystackKeys.
         */
        void addKey(const string& root, const BSONElement& e, BSONObjSet* keys) {
            BSONObjBuilder buf;
            buf.append("", root);

            if (e.eoo())
                buf.appendNull("");
            else
                buf.appendAs(e, "");

            keys->insert(buf.obj());
        }

    }

    void HaystackKeyGenerator::getKeys( const BSONObj& obj, BSONObjSet* keys) const {

        BSONElement loc = obj.getFieldDotted(_geoField);

        if (loc.eoo()) { return; }

        uassert(16775, "latlng not an array", loc.isABSONObj());
        string root;
        {
            BSONObjIterator i(loc.Obj());
            BSONElement x = i.next();
            BSONElement y = i.next();
            root = makeHaystackString(hashHaystackElement(x, _bucketSize),
                                      hashHaystackElement(y, _bucketSize));
        }

        verify(_otherFields.size() == 1);

        BSONElementSet all;

        // This is getFieldsDotted (plural not singular) since the object we're indexing
        // may be an array.
        obj.getFieldsDotted(_otherFields[0], all);

        if (all.size() == 0) {
            // We're indexing a document that doesn't have the secondary non-geo field present.
            // XXX: do we want to add this even if all.size() > 0?  result:empty search terms
            // match everything instead of only things w/empty search terms)
            addKey(root, BSONElement(), keys);
        } else {
            // Ex:If our secondary field is type: "foo" or type: {a:"foo", b:"bar"},
            // all.size()==1.  We can query on the complete field.
            // Ex: If our secondary field is type: ["A", "B"] all.size()==2 and all has values
            // "A" and "B".  The query looks for any of the fields in the array.
            for (BSONElementSet::iterator i = all.begin(); i != all.end(); ++i) {
                addKey(root, *i, keys);
            }
        }
    }

    // static
    int HaystackKeyGenerator::hashHaystackElement(const BSONElement& e, double bucketSize) {
        uassert(16776, "geo field is not a number", e.isNumber());
        double d = e.numberDouble();
        d += 180;
        d /= bucketSize;
        return static_cast<int>(d);
    }

    // static
    std::string HaystackKeyGenerator::makeHaystackString(int hashedX, int hashedY) {
        mongoutils::str::stream ss;
        ss << hashedX << "_" << hashedY;
        return ss;
    }

}  // namespace mongo
