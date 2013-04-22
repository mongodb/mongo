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

#include "pch.h"

#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/namespace-inl.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/index.h"
#include "mongo/util/startup_test.h"
#include "mongo/db/commands.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/btreecursor.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/matcher.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/geo/core.h"
#include "mongo/db/geo/geonear.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/util/timer.h"

namespace mongo {

    enum GeoDistType {
        GEO_PLAIN,
        GEO_SPHERE
    };

    const string GEO2DNAME = "2d";

    class Geo2dType : public IndexType {
    public:
        virtual ~Geo2dType() { }

        Geo2dType(const IndexPlugin *plugin, const IndexSpec* spec) : IndexType(plugin, spec) {
            BSONObjIterator i(spec->keyPattern);
            while (i.more()) {
                BSONElement e = i.next();
                if (e.type() == String && GEO2DNAME == e.valuestr()) {
                    uassert(13022, "can't have 2 geo field", _geo.size() == 0);
                    uassert(13023, "2d has to be first in index", _other.size() == 0);
                    _geo = e.fieldName();
                } else {
                    int order = 1;
                    if (e.isNumber()) {
                        order = static_cast<int>(e.Number());
                    }
                    _other.push_back(make_pair(e.fieldName(), order));
                }
            }
            uassert(13024, "no geo field specified", _geo.size());

            double bits =  configValueWithDefault(spec, "bits", 26); // for lat/long, ~ 1ft
            uassert(13028, "bits in geo index must be between 1 and 32", bits > 0 && bits <= 32);

            GeoHashConverter::Parameters params;
            params.bits = static_cast<unsigned>(bits);
            params.max = configValueWithDefault(spec, "max", 180.0);
            params.min = configValueWithDefault(spec, "min", -180.0);
            double numBuckets = (1024 * 1024 * 1024 * 4.0);
            params.scaling = numBuckets / (params.max - params.min);

            _geoHashConverter.reset(new GeoHashConverter(params));
        }

        // XXX: what does this do
        virtual BSONObj fixKey(const BSONObj& in) {
            if (in.firstElement().type() == BinData)
                return in;

            BSONObjBuilder b(in.objsize() + 16);

            if (in.firstElement().isABSONObj())
                _geoHashConverter->hash(in.firstElement().embeddedObject()).appendToBuilder(&b, "");
            else if (in.firstElement().type() == String)
                GeoHash(in.firstElement().valuestr()).appendToBuilder(&b, "");
            else if (in.firstElement().type() == RegEx)
                GeoHash(in.firstElement().regex()).appendToBuilder(&b, "");
            else
                return in;

            BSONObjIterator i(in);
            i.next();
            while (i.more())
                b.append(i.next());
            return b.obj();
        }

        /** Finds the key objects to put in an index */
        virtual void getKeys(const BSONObj& obj, BSONObjSet& keys) const {
            getKeys(obj, &keys, NULL);
        }

        /** Finds all locations in a geo-indexed object */
        // TODO:  Can we just return references to the locs, if they won't change?
        void getKeys(const BSONObj& obj, vector<BSONObj>& locs) const {
            getKeys(obj, NULL, &locs);
        }

        /** Finds the key objects and/or locations for a geo-indexed object */
        void getKeys(const BSONObj &obj, BSONObjSet* keys, vector<BSONObj>* locs) const {
            BSONElementMSet bSet;

            // Get all the nested location fields, but don't return individual elements from
            // the last array, if it exists.
            obj.getFieldsDotted(_geo.c_str(), bSet, false);

            if (bSet.empty())
                return;

            for (BSONElementMSet::iterator setI = bSet.begin(); setI != bSet.end(); ++setI) {
                BSONElement geo = *setI;

                GEODEBUG("Element " << geo << " found for query " << _geo.c_str());

                if (geo.eoo() || !geo.isABSONObj())
                    continue;

                //
                // Grammar for location lookup:
                // locs ::= [loc,loc,...,loc]|{<k>:loc,<k>:loc,...,<k>:loc}|loc
                // loc  ::= { <k1> : #, <k2> : # }|[#, #]|{}
                //
                // Empty locations are ignored, preserving single-location semantics
                //

                BSONObj embed = geo.embeddedObject();
                if (embed.isEmpty())
                    continue;

                // Differentiate between location arrays and locations
                // by seeing if the first element value is a number
                bool singleElement = embed.firstElement().isNumber();

                BSONObjIterator oi(embed);

                while (oi.more()) {
                    BSONObj locObj;

                    if (singleElement) {
                        locObj = embed;
                    } else {
                        BSONElement locElement = oi.next();

                        uassert(13654, str::stream() << "location object expected, location "
                                                       "array not in correct format",
                                locElement.isABSONObj());

                        locObj = locElement.embeddedObject();
                        if(locObj.isEmpty())
                            continue;
                    }

                    BSONObjBuilder b(64);

                    // Remember the actual location object if needed
                    if (locs)
                        locs->push_back(locObj);

                    // Stop if we don't need to get anything but location objects
                    if (!keys) {
                        if (singleElement) break;
                        else continue;
                    }

                    _geoHashConverter->hash(locObj, &obj).appendToBuilder(&b, "");

                    // Go through all the other index keys
                    for (vector<pair<string, int> >::const_iterator i = _other.begin();
                         i != _other.end(); ++i) {
                        // Get *all* fields for the index key
                        BSONElementSet eSet;
                        obj.getFieldsDotted(i->first, eSet);

                        if (eSet.size() == 0)
                            b.appendAs(_spec->missingField(), "");
                        else if (eSet.size() == 1)
                            b.appendAs(*(eSet.begin()), "");
                        else {
                            // If we have more than one key, store as an array of the objects
                            BSONArrayBuilder aBuilder;

                            for (BSONElementSet::iterator ei = eSet.begin(); ei != eSet.end();
                                 ++ei) {
                                aBuilder.append(*ei);
                            }

                            b.append("", aBuilder.arr());
                        }
                    }
                    keys->insert(b.obj());
                    if(singleElement) break;
                }
            }
        }

        const IndexDetails* getDetails() const { return _spec->getDetails(); }

        const GeoHashConverter& getConverter() const { return *_geoHashConverter; }

        // XXX: make private with a getter
        string _geo;
        vector<pair<string, int> > _other;
    private:
        double configValueWithDefault(const IndexSpec* spec, const string& name, double def) {
            BSONElement e = spec->info[name];
            if (e.isNumber()) {
                return e.numberDouble();
            }
            return def;
        }

        scoped_ptr<GeoHashConverter> _geoHashConverter;
    };

    class Geo2dPlugin : public IndexPlugin {
    public:
        Geo2dPlugin() : IndexPlugin(GEO2DNAME) { }

        virtual IndexType* generate(const IndexSpec* spec) const {
            return new Geo2dType(this, spec);
        }
    } geo2dplugin;

    void __forceLinkGeoPlugin() {
        geo2dplugin.getName();
    }

    struct GeoUnitTest : public StartupTest {
        int round(double d) {
            return (int)(.5 + (d * 1000));
        }

#define GEOHEQ(a,b) if (a.toString() != b){ cout << "[" << a.toString() << "] != [" << b << "]" << endl; verify(a == GeoHash(b)); }

        void run() {
            verify(!GeoHash::isBitSet(0, 0));
            verify(!GeoHash::isBitSet(0, 31));
            verify(GeoHash::isBitSet(1, 31));

            IndexSpec i(BSON("loc" << "2d"));
            Geo2dType g(&geo2dplugin, &i);
            const GeoHashConverter &conv = g.getConverter();

            {
                double x = 73.01212;
                double y = 41.352964;
                BSONObj in = BSON("x" << x << "y" << y);
                GeoHash h = conv.hash(in);
                BSONObj out = conv.unhashToBSONObj(h);
                verify(round(x) == round(out["x"].number()));
                verify(round(y) == round(out["y"].number()));
                verify(round(in["x"].number()) == round(out["x"].number()));
                verify(round(in["y"].number()) == round(out["y"].number()));
            }
            {
                double x = -73.01212;
                double y = 41.352964;
                BSONObj in = BSON("x" << x << "y" << y);
                GeoHash h = conv.hash(in);
                BSONObj out = conv.unhashToBSONObj(h);
                verify(round(x) == round(out["x"].number()));
                verify(round(y) == round(out["y"].number()));
                verify(round(in["x"].number()) == round(out["x"].number()));
                verify(round(in["y"].number()) == round(out["y"].number()));
            }
            {
                GeoHash h("0000");
                h.move(0, 1);
                GEOHEQ(h, "0001");
                h.move(0, -1);
                GEOHEQ(h, "0000");

                h = GeoHash("0001");
                h.move(0, 1);
                GEOHEQ(h, "0100");
                h.move(0, -1);
                GEOHEQ(h, "0001");

                h = GeoHash("0000");
                h.move(1, 0);
                GEOHEQ(h, "0010");
            }
            {
                Box b(5, 5, 2);
                verify("(5,5) -->> (7,7)" == b.toString());
            }
            {
                GeoHash a = conv.hash(1, 1);
                GeoHash b = conv.hash(4, 5);
                verify(5 == (int)(conv.distanceBetweenHashes(a, b)));
                a = conv.hash(50, 50);
                b = conv.hash(42, 44);
                verify(round(10) == round(conv.distanceBetweenHashes(a, b)));
            }
            {
                GeoHash x("0000");
                verify(0 == x.getHash());
                x = GeoHash(0, 1, 32);
                GEOHEQ(x, "0000000000000000000000000000000000000000000000000000000000000001")
                    
                verify(GeoHash("1100").hasPrefix(GeoHash("11")));
                verify(!GeoHash("1000").hasPrefix(GeoHash("11")));
            }
            {
                GeoHash x("1010");
                GEOHEQ(x, "1010");
                GeoHash y = x + "01";
                GEOHEQ(y, "101001");
            }
            {
                GeoHash a = conv.hash(5, 5);
                GeoHash b = conv.hash(5, 7);
                GeoHash c = conv.hash(100, 100);
                BSONObj oa = a.wrap();
                BSONObj ob = b.wrap();
                BSONObj oc = c.wrap();
                verify(oa.woCompare(ob) < 0);
                verify(oa.woCompare(oc) < 0);
            }
            {
                GeoHash x("000000");
                x.move(-1, 0);
                GEOHEQ(x, "101010");
                x.move(1, -1);
                GEOHEQ(x, "010101");
                x.move(0, 1);
                GEOHEQ(x, "000000");
            }
            {
                GeoHash prefix("110011000000");
                GeoHash entry( "1100110000011100000111000001110000011100000111000001000000000000");
                verify(!entry.hasPrefix(prefix));
                entry = GeoHash("1100110000001100000111000001110000011100000111000001000000000000");
                verify(entry.toString().find(prefix.toString()) == 0);
                verify(entry.hasPrefix(GeoHash("1100")));
                verify(entry.hasPrefix(prefix));
            }
            {
                GeoHash a = conv.hash(50, 50);
                GeoHash b = conv.hash(48, 54);
                verify(round(4.47214) == round(conv.distanceBetweenHashes(a, b)));
            }
            {
                Box b(Point(29.762283, -95.364271), Point(29.764283000000002, -95.36227099999999));
                verify(b.inside(29.763, -95.363));
                verify(! b.inside(32.9570255, -96.1082497));
                verify(! b.inside(32.9570255, -96.1082497, .01));
            }
            {
                GeoHash a("11001111");
                verify(GeoHash("11") == a.commonPrefix(GeoHash("11")));
                verify(GeoHash("11") == a.commonPrefix(GeoHash("11110000")));
            }
            {
                int N = 10000;
#if 0  // XXX: we want to make sure the two unhash versions both work, but private.
                {
                    Timer t;
                    for (int i = 0; i < N; i++) {
                        unsigned x = (unsigned)rand();
                        unsigned y = (unsigned)rand();
                        GeoHash h(x, y);
                        unsigned a, b;
                        h.unhash(&a, &b);
                        verify(a == x);
                        verify(b == y);
                    }
                    //cout << "slow: " << t.millis() << endl;
                }
#endif
                {
                    Timer t;
                    for (int i=0; i<N; i++) {
                        unsigned x = (unsigned)rand();
                        unsigned y = (unsigned)rand();
                        GeoHash h(x, y);
                        unsigned a, b;
                        h.unhash(&a, &b);
                        verify(a == x);
                        verify(b == y);
                    }
                    //cout << "fast: " << t.millis() << endl;
                }

            }

            {
                // see http://en.wikipedia.org/wiki/Great-circle_distance#Worked_example
                {
                    Point BNA (-86.67, 36.12);
                    Point LAX (-118.40, 33.94);

                    double dist1 = spheredist_deg(BNA, LAX);
                    double dist2 = spheredist_deg(LAX, BNA);

                    // target is 0.45306
                    verify(0.45305 <= dist1 && dist1 <= 0.45307);
                    verify(0.45305 <= dist2 && dist2 <= 0.45307);
                }
                {
                    Point BNA (-1.5127, 0.6304);
                    Point LAX (-2.0665, 0.5924);

                    double dist1 = spheredist_rad(BNA, LAX);
                    double dist2 = spheredist_rad(LAX, BNA);

                    // target is 0.45306
                    verify(0.45305 <= dist1 && dist1 <= 0.45307);
                    verify(0.45305 <= dist2 && dist2 <= 0.45307);
                }
                {
                    Point JFK (-73.77694444, 40.63861111);
                    Point LAX (-118.40, 33.94);

                    const double EARTH_RADIUS_KM = 6371;
                    const double EARTH_RADIUS_MILES = EARTH_RADIUS_KM * 0.621371192;
                    double dist = spheredist_deg(JFK, LAX) * EARTH_RADIUS_MILES;
                    verify(dist > 2469 && dist < 2470);
                }
                {
                    Point BNA (-86.67, 36.12);
                    Point LAX (-118.40, 33.94);
                    Point JFK (-73.77694444, 40.63861111);
                    verify(spheredist_deg(BNA, BNA) < 1e-6);
                    verify(spheredist_deg(LAX, LAX) < 1e-6);
                    verify(spheredist_deg(JFK, JFK) < 1e-6);

                    Point zero (0, 0);
                    Point antizero (0,-180);

                    // these were known to cause NaN
                    verify(spheredist_deg(zero, zero) < 1e-6);
                    verify(fabs(M_PI-spheredist_deg(zero, antizero)) < 1e-6);
                    verify(fabs(M_PI-spheredist_deg(antizero, zero)) < 1e-6);
                }
            }
        }
    } geoUnitTest;
}
