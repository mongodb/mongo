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
#include "mongo/db/btreecursor.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/index/haystack_access_method_internal.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    static const string GEOSEARCHNAME = "geoHaystack";

    HaystackAccessMethod::HaystackAccessMethod(IndexDescriptor* descriptor)
        : BtreeBasedAccessMethod(descriptor) {

        BSONElement e = descriptor->getInfoElement("bucketSize");
        uassert(16777, "need bucketSize", e.isNumber());
        _bucketSize = e.numberDouble();
        uassert(16769, "bucketSize cannot be zero", _bucketSize != 0.0);

        // Example:
        // db.foo.ensureIndex({ pos : "geoHaystack", type : 1 }, { bucketSize : 1 })
        BSONObjIterator i(descriptor->keyPattern());
        while (i.more()) {
            BSONElement e = i.next();
            if (e.type() == String && GEOSEARCHNAME == e.valuestr()) {
                uassert(16770, "can't have more than one geo field", _geoField.size() == 0);
                uassert(16771, "the geo field has to be first in index",
                        _otherFields.size() == 0);
                _geoField = e.fieldName();
            } else {
                uassert(16772, "geoSearch can only have 1 non-geo field for now",
                        _otherFields.size() == 0);
                _otherFields.push_back(e.fieldName());
            }
        }

        uassert(16773, "no geo field specified", _geoField.size());
        uassert(16774, "no non-geo fields specified", _otherFields.size());
    }

    void HaystackAccessMethod::getKeys(const BSONObj& obj, BSONObjSet* keys) {
        BSONElement loc = obj.getFieldDotted(_geoField);

        if (loc.eoo()) { return; }

        uassert(16775, "latlng not an array", loc.isABSONObj());
        string root;
        {
            BSONObjIterator i(loc.Obj());
            BSONElement x = i.next();
            BSONElement y = i.next();
            root = makeString(hash(x), hash(y));
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

    int HaystackAccessMethod::hash(const BSONElement& e) const {
        uassert(16776, "geo field is not a number", e.isNumber());
        double d = e.numberDouble();
        d += 180;
        d /= _bucketSize;
        return static_cast<int>(d);
    }

    string HaystackAccessMethod::makeString(int hashedX, int hashedY) const {
        stringstream ss;
        ss << hashedX << "_" << hashedY;
        return ss.str();
    }

    // Build a new BSONObj with root in it.  If e is non-empty, append that to the key.  Insert
    // the BSONObj into keys.
    void HaystackAccessMethod::addKey(const string& root, const BSONElement& e,
                                      BSONObjSet* keys) const {
        BSONObjBuilder buf;
        buf.append("", root);

        if (e.eoo())
            buf.appendNull("");
        else
            buf.appendAs(e, "");

        keys->insert(buf.obj());
    }

    Status HaystackAccessMethod::newCursor(IndexCursor** out) {
        return Status(ErrorCodes::IllegalOperation, "Unimplemented seek called on Haystack");
    }

    void HaystackAccessMethod::searchCommand(const BSONObj& nearObj, double maxDistance,
                                             const BSONObj& search, BSONObjBuilder* result,
                                             unsigned limit) {
        Timer t;

        LOG(1) << "SEARCH near:" << nearObj << " maxDistance:" << maxDistance
               << " search: " << search << endl;
        int x, y;
        {
            BSONObjIterator i(nearObj);
            x = hash(i.next());
            y = hash(i.next());
        }
        int scale = static_cast<int>(ceil(maxDistance / _bucketSize));

        GeoHaystackSearchHopper hopper(nearObj, maxDistance, limit, _geoField);

        long long btreeMatches = 0;

        for (int a = -scale; a <= scale && !hopper.limitReached(); ++a) {
            for (int b = -scale; b <= scale && !hopper.limitReached(); ++b) {
                BSONObjBuilder bb;
                bb.append("", makeString(x + a, y + b));

                for (unsigned i = 0; i < _otherFields.size(); i++) {
                    // See if the non-geo field we're indexing on is in the provided search term.
                    BSONElement e = search.getFieldDotted(_otherFields[i]);
                    if (e.eoo())
                        bb.appendNull("");
                    else
                        bb.appendAs(e, "");
                }

                BSONObj key = bb.obj();

                GEOQUADDEBUG("KEY: " << key);

                // TODO(hk): this keeps a set of all DiskLoc seen in this pass so that we don't
                // consider the element twice.  Do we want to instead store a hash of the set?
                // Is this often big?
                unordered_set<DiskLoc, DiskLoc::Hasher> thisPass;


                scoped_ptr<BtreeCursor> cursor(BtreeCursor::make(nsdetails(_descriptor->parentNS()),
                                                                 _descriptor->getOnDisk(),
                                                                 key,
                                                                 key,
                                                                 true,
                                                                 1));
                while (cursor->ok() && !hopper.limitReached()) {
                    pair<unordered_set<DiskLoc, DiskLoc::Hasher>::iterator, bool> p
                        = thisPass.insert(cursor->currLoc());
                    // If a new element was inserted (haven't seen the DiskLoc before), p.second
                    // is true.
                    if (p.second) {
                        hopper.consider(cursor->currLoc());
                        GEOQUADDEBUG("\t" << cursor->current());
                        btreeMatches++;
                    }
                    cursor->advance();
                }
            }
        }

        BSONArrayBuilder arr(result->subarrayStart("results"));
        int num = hopper.appendResultsTo(&arr);
        arr.done();

        {
            BSONObjBuilder b(result->subobjStart("stats"));
            b.append("time", t.millis());
            b.appendNumber("btreeMatches", btreeMatches);
            b.append("n", num);
            b.done();
        }
    }

}  // namespace mongo
