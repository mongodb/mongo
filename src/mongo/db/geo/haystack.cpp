// XXX THIS FILE IS DEPRECATED.  PLEASE DON'T MODIFY.
// db/geo/haystack.cpp

/**
 *    Copyright (C) 2008-2012 10gen Inc.
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

#include "pch.h"

#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/namespace-inl.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/index.h"
#include "mongo/db/commands.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/btreecursor.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/matcher.h"
#include "mongo/db/geo/core.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/util/timer.h"

/**
 * Provides the geoHaystack index type and the command "geoSearch."
 * Examines all documents in a given radius of a given point.
 * Returns all documents that match a given search restriction.
 * See http://dochub.mongodb.org/core/haystackindexes
 *
 * Use when you want to look for restaurants within 25 miles with a certain name.
 * Don't use when you want to find the closest open restaurants; see 2d.cpp for that.
 */
namespace mongo {
    static const string GEOSEARCHNAME = "geoHaystack";

    class GeoHaystackSearchHopper {
    public:
        /**
         * Constructed with a point, a max distance from that point, and a max number of
         * matched points to store.
         * @param n  The centroid that we're searching
         * @param maxDistance  The maximum distance to consider from that point
         * @param limit  The maximum number of results to return
         * @param geoField  Which field in the provided DiskLoc has the point to test.
         */
        GeoHaystackSearchHopper(const BSONObj& nearObj, double maxDistance, unsigned limit,
                                const string& geoField)
            : _near(nearObj), _maxDistance(maxDistance), _limit(limit), _geoField(geoField) { }

        // Consider the point in loc, and keep it if it's within _maxDistance (and we have space for
        // it)
        void consider(const DiskLoc& loc) {
            if (limitReached()) return;
            Point p(loc.obj().getFieldDotted(_geoField));
            if (distance(_near, p) > _maxDistance)
                return;
            _locs.push_back(loc);
        }

        int appendResultsTo(BSONArrayBuilder* b) {
            for (unsigned i = 0; i <_locs.size(); i++)
                b->append(_locs[i].obj());
            return _locs.size();
        }

        // Have we stored as many points as we can?
        const bool limitReached() const {
            return _locs.size() >= _limit;
        }
    private:
        Point _near;
        double _maxDistance;
        unsigned _limit;
        const string _geoField;
        vector<DiskLoc> _locs;
    };

    /**
     * Provides the IndexType for geoSearch.
     * Maps (lat, lng) to the bucketSize-sided square bucket that contains it.
     * Usage:
     * db.foo.ensureIndex({ pos : "geoHaystack", type : 1 }, { bucketSize : 1 })
     *   pos is the name of the field to be indexed that has lat/lng data in an array.
     *   type is the name of the secondary field to be indexed. 
     *   bucketSize specifies the dimension of the square bucket for the data in pos.
     * ALL fields are mandatory.
     */
    class GeoHaystackSearchIndex : public IndexType {
    public:
        GeoHaystackSearchIndex(const IndexPlugin* plugin, const IndexSpec* spec)
            : IndexType(plugin, spec) {

            BSONElement e = spec->info["bucketSize"];
            uassert(13321, "need bucketSize", e.isNumber());
            _bucketSize = e.numberDouble();
            uassert(16455, "bucketSize cannot be zero", _bucketSize != 0.0);

            // Example:
            // db.foo.ensureIndex({ pos : "geoHaystack", type : 1 }, { bucketSize : 1 })
            BSONObjIterator i(spec->keyPattern);
            while (i.more()) {
                BSONElement e = i.next();
                if (e.type() == String && GEOSEARCHNAME == e.valuestr()) {
                    uassert(13314, "can't have more than one geo field", _geoField.size() == 0);
                    uassert(13315, "the geo field has to be first in index",
                            _otherFields.size() == 0);
                    _geoField = e.fieldName();
                } else {
                    // TODO(hk): Do we want to do any checking on e.type and e.valuestr?
                    uassert(13326, "geoSearch can only have 1 non-geo field for now",
                            _otherFields.size() == 0);
                    _otherFields.push_back(e.fieldName());
                }
            }

            uassert(13316, "no geo field specified", _geoField.size());
            // XXX: Fix documentation that says the other field is optional; code says it's mandatory.
            uassert(13317, "no non-geo fields specified", _otherFields.size());
        }

        void getKeys(const BSONObj &obj, BSONObjSet &keys) const {
            BSONElement loc = obj.getFieldDotted(_geoField);
            if (loc.eoo())
                return;

            uassert(13323, "latlng not an array", loc.isABSONObj());
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

        void searchCommand(NamespaceDetails* nsd,
                            const BSONObj& n /*near*/, double maxDistance, const BSONObj& search,
                            BSONObjBuilder& result, unsigned limit) {
            Timer t;

            LOG(1) << "SEARCH near:" << n << " maxDistance:" << maxDistance
                   << " search: " << search << endl;
            int x, y;
            {
                BSONObjIterator i(n);
                x = hash(i.next());
                y = hash(i.next());
            }
            int scale = static_cast<int>(ceil(maxDistance / _bucketSize));

            GeoHaystackSearchHopper hopper(n, maxDistance, limit, _geoField);

            long long btreeMatches = 0;

            // TODO(hk): Consider starting with a (or b)=0, then going to a=+-1, then a=+-2, etc.
            // Would want a HaystackKeyIterator or similar for this, but it'd be a nice
            // encapsulation allowing us to S2-ify this trivially/abstract the key details.
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
                    set<DiskLoc> thisPass;

                    // Lookup from key to key, inclusive.
                    scoped_ptr<BtreeCursor> cursor(BtreeCursor::make(nsd,
                                                                     *getDetails(),
                                                                     key,
                                                                     key,
                                                                     true,
                                                                     1));
                    while (cursor->ok() && !hopper.limitReached()) {
                        pair<set<DiskLoc>::iterator, bool> p = thisPass.insert(cursor->currLoc());
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

            BSONArrayBuilder arr(result.subarrayStart("results"));
            int num = hopper.appendResultsTo(&arr);
            arr.done();

            {
                BSONObjBuilder b(result.subobjStart("stats"));
                b.append("time", t.millis());
                b.appendNumber("btreeMatches", btreeMatches);
                b.append("n", num);
                b.done();
            }
        }

        const IndexDetails* getDetails() const {
            return _spec->getDetails();
        }
    private:
        // TODO(hk): consider moving hash/unhash/makeString out
        int hash(const BSONElement& e) const {
            uassert(13322, "geo field is not a number", e.isNumber());
            return hash(e.numberDouble());
        }

        int hash(double d) const {
            d += 180;
            d /= _bucketSize;
            return static_cast<int>(d);
        }

        string makeString(int hashedX, int hashedY) const {
            stringstream ss;
            ss << hashedX << "_" << hashedY;
            return ss.str();
        }

        // Build a new BSONObj with root in it.  If e is non-empty, append that to the key.  Insert
        // the BSONObj into keys.
        void addKey(const string& root, const BSONElement& e, BSONObjSet& keys) const {
            BSONObjBuilder buf;
            buf.append("", root);

            if (e.eoo())
                buf.appendNull("");
            else
                buf.appendAs(e, "");

            keys.insert(buf.obj());
        }

        string _geoField;
        vector<string> _otherFields;
        double _bucketSize;
    };

    class GeoHaystackSearchIndexPlugin : public IndexPlugin {
    public:
        GeoHaystackSearchIndexPlugin() : IndexPlugin(GEOSEARCHNAME) { }

        virtual IndexType* generate(const IndexSpec* spec) const {
            return new GeoHaystackSearchIndex(this, spec);
        }
    } nameIndexPlugin;

    class GeoHaystackSearchCommand : public Command {
    public:
        GeoHaystackSearchCommand() : Command("geoSearch") {}

        virtual LockType locktype() const { return READ; }
        bool slaveOk() const { return true; }
        bool slaveOverrideOk() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }
        bool run(const string& dbname, BSONObj& cmdObj, int,
                 string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string ns = dbname + "." + cmdObj.firstElement().valuestr();

            NamespaceDetails *nsd = nsdetails(ns);
            if (NULL == nsd) {
                errmsg = "can't find ns";
                return false;
            }

            vector<int> idxs;
            nsd->findIndexByType(GEOSEARCHNAME, idxs);
            if (idxs.size() == 0) {
                errmsg = "no geoSearch index";
                return false;
            }
            if (idxs.size() > 1) {
                errmsg = "more than 1 geosearch index";
                return false;
            }

            BSONElement nearElt = cmdObj["near"];
            BSONElement maxDistance = cmdObj["maxDistance"];
            BSONElement search = cmdObj["search"];

            uassert(13318, "near needs to be an array", nearElt.isABSONObj());
            uassert(13319, "maxDistance needs a number", maxDistance.isNumber());
            uassert(13320, "search needs to be an object", search.type() == Object);

            unsigned limit = 50;
            if (cmdObj["limit"].isNumber())
                limit = static_cast<unsigned>(cmdObj["limit"].numberInt());

            int idxNum = idxs[0];
            auto_ptr<IndexDescriptor> desc(CatalogHack::getDescriptor(nsd, idxNum));
            auto_ptr<HaystackAccessMethod> ham(new HaystackAccessMethod(desc.get()));
            ham->searchCommand(nearElt.Obj(), maxDistance.numberDouble(), search.Obj(),
                               &result, limit);
            return 1;
        }
    } nameSearchCommand;
}
