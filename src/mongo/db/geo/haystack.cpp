// db/geo/haystack.cpp

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
#include "../namespace-inl.h"
#include "../jsobj.h"
#include "../index.h"
#include "../commands.h"
#include "../pdfile.h"
#include "../btree.h"
#include "../curop-inl.h"
#include "../matcher.h"
#include "core.h"
#include "../../util/timer.h"

#define GEOQUADDEBUG(x)
//#define GEOQUADDEBUG(x) cout << x << endl

/**
 * this is a geo based search piece, which is different than regular geo lookup
 * this is useful when you want to look for something within a region where the ratio is low
 * works well for search for restaurants withing 25 miles with a certain name
 * should not be used for finding the closest restaurants that are open
 */
namespace mongo {

    string GEOSEARCHNAME = "geoHaystack";

    class GeoHaystackSearchHopper {
    public:
        GeoHaystackSearchHopper( const BSONObj& n , double maxDistance , unsigned limit , const string& geoField )
            : _near( n ) , _maxDistance( maxDistance ) , _limit( limit ) , _geoField(geoField) {

        }

        void got( const DiskLoc& loc ) {
            Point p( loc.obj().getFieldDotted( _geoField ) );
            if ( _near.distance( p ) > _maxDistance )
                return;
            _locs.push_back( loc );
        }

        int append( BSONArrayBuilder& b ) {
            for ( unsigned i=0; i<_locs.size() && i<_limit; i++ )
                b.append( _locs[i].obj() );
            return _locs.size();
        }

        Point _near;
        double _maxDistance;
        unsigned _limit;
        string _geoField;

        vector<DiskLoc> _locs;
    };

    class GeoHaystackSearchIndex : public IndexType {

    public:

        GeoHaystackSearchIndex( const IndexPlugin* plugin , const IndexSpec* spec )
            : IndexType( plugin , spec ) {

            BSONElement e = spec->info["bucketSize"];
            uassert( 13321 , "need bucketSize" , e.isNumber() );
            _bucketSize = e.numberDouble();

            BSONObjBuilder orderBuilder;

            BSONObjIterator i( spec->keyPattern );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( e.type() == String && GEOSEARCHNAME == e.valuestr() ) {
                    uassert( 13314 , "can't have 2 geo fields" , _geo.size() == 0 );
                    uassert( 13315 , "2d has to be first in index" , _other.size() == 0 );
                    _geo = e.fieldName();
                }
                else {
                    _other.push_back( e.fieldName() );
                }
                orderBuilder.append( "" , 1 );
            }

            uassert( 13316 , "no geo field specified" , _geo.size() );
            uassert( 13317 , "no other fields specified" , _other.size() );
            uassert( 13326 , "quadrant search can only have 1 other field for now" , _other.size() == 1 );
            _order = orderBuilder.obj();
        }

        int hash( const BSONElement& e ) const {
            uassert( 13322 , "not a number" , e.isNumber() );
            return hash( e.numberDouble() );
        }

        int hash( double d ) const {
            d += 180;
            d /= _bucketSize;
            return (int)d;
        }

        string makeString( int hashedX , int hashedY ) const {
            stringstream ss;
            ss << hashedX << "_" << hashedY;
            return ss.str();
        }

        void _add( const BSONObj& obj, const string& root , const BSONElement& e , BSONObjSet& keys ) const {
            BSONObjBuilder buf;
            buf.append( "" , root );
            if ( e.eoo() )
                buf.appendNull( "" );
            else
                buf.appendAs( e , "" );

            BSONObj key = buf.obj();
            GEOQUADDEBUG( obj << "\n\t" << root << "\n\t" << key );
            keys.insert( key );
        }

        void getKeys( const BSONObj &obj, BSONObjSet &keys ) const {

            BSONElement loc = obj.getFieldDotted( _geo );
            if ( loc.eoo() )
                return;

            uassert( 13323 , "latlng not an array" , loc.isABSONObj() );
            string root;
            {
                BSONObjIterator i( loc.Obj() );
                BSONElement x = i.next();
                BSONElement y = i.next();
                root = makeString( hash(x) , hash(y) );
            }


            verify( _other.size() == 1 );

            BSONElementSet all;
            obj.getFieldsDotted( _other[0] , all );

            if ( all.size() == 0 ) {
                _add( obj , root , BSONElement() , keys );
            }
            else {
                for ( BSONElementSet::iterator i=all.begin(); i!=all.end(); ++i ) {
                    _add( obj , root , *i , keys );
                }
            }

        }

        shared_ptr<Cursor> newCursor( const BSONObj& query , const BSONObj& order , int numWanted ) const {
            shared_ptr<Cursor> c;
            verify(0);
            return c;
        }

        void searchCommand( NamespaceDetails* nsd , int idxNo ,
                            const BSONObj& n /*near*/ , double maxDistance , const BSONObj& search ,
                            BSONObjBuilder& result , unsigned limit ) {

            Timer t;

            log(1) << "SEARCH near:" << n << " maxDistance:" << maxDistance << " search: " << search << endl;
            int x,y;
            {
                BSONObjIterator i( n );
                x = hash( i.next() );
                y = hash( i.next() );
            }
            int scale = (int)ceil( maxDistance / _bucketSize );

            GeoHaystackSearchHopper hopper(n,maxDistance,limit,_geo);

            long long btreeMatches = 0;

            for ( int a=-scale; a<=scale; a++ ) {
                for ( int b=-scale; b<=scale; b++ ) {

                    BSONObjBuilder bb;
                    bb.append( "" , makeString( x + a , y + b ) );
                    for ( unsigned i=0; i<_other.size(); i++ ) {
                        BSONElement e = search.getFieldDotted( _other[i] );
                        if ( e.eoo() )
                            bb.appendNull( "" );
                        else
                            bb.appendAs( e , "" );
                    }

                    BSONObj key = bb.obj();

                    GEOQUADDEBUG( "KEY: " << key );

                    set<DiskLoc> thisPass;
                    scoped_ptr<BtreeCursor> cursor( BtreeCursor::make( nsd , idxNo , *getDetails() , key , key , true , 1 ) );
                    while ( cursor->ok() ) {
                        pair<set<DiskLoc>::iterator, bool> p = thisPass.insert( cursor->currLoc() );
                        if ( p.second ) {
                            hopper.got( cursor->currLoc() );
                            GEOQUADDEBUG( "\t" << cursor->current() );
                            btreeMatches++;
                        }
                        cursor->advance();
                    }
                }

            }

            BSONArrayBuilder arr( result.subarrayStart( "results" ) );
            int num = hopper.append( arr );
            arr.done();

            {
                BSONObjBuilder b( result.subobjStart( "stats" ) );
                b.append( "time" , t.millis() );
                b.appendNumber( "btreeMatches" , btreeMatches );
                b.append( "n" , num );
                b.done();
            }
        }

        const IndexDetails* getDetails() const {
            return _spec->getDetails();
        }

        string _geo;
        vector<string> _other;

        BSONObj _order;

        double _bucketSize;
    };

    class GeoHaystackSearchIndexPlugin : public IndexPlugin {
    public:
        GeoHaystackSearchIndexPlugin() : IndexPlugin( GEOSEARCHNAME ) {
        }

        virtual IndexType* generate( const IndexSpec* spec ) const {
            return new GeoHaystackSearchIndex( this , spec );
        }

    } nameIndexPlugin;


    class GeoHaystackSearchCommand : public Command {
    public:
        GeoHaystackSearchCommand() : Command( "geoSearch" ) {}
        virtual LockType locktype() const { return READ; }
        bool slaveOk() const { return true; }
        bool slaveOverrideOk() const { return true; }
        bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {

            string ns = dbname + "." + cmdObj.firstElement().valuestr();

            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( ! d ) {
                errmsg = "can't find ns";
                return false;
            }

            vector<int> idxs;
            d->findIndexByType( GEOSEARCHNAME , idxs );
            if ( idxs.size() == 0 ) {
                errmsg = "no geoSearch index";
                return false;
            }
            if ( idxs.size() > 1 ) {
                errmsg = "more than 1 geosearch index";
                return false;
            }

            int idxNum = idxs[0];

            IndexDetails& id = d->idx( idxNum );
            GeoHaystackSearchIndex * si = (GeoHaystackSearchIndex*)id.getSpec().getType();
            verify( &id == si->getDetails() );

            BSONElement n = cmdObj["near"];
            BSONElement maxDistance = cmdObj["maxDistance"];
            BSONElement search = cmdObj["search"];

            uassert( 13318 , "near needs to be an array" , n.isABSONObj() );
            uassert( 13319 , "maxDistance needs a number" , maxDistance.isNumber() );
            uassert( 13320 , "search needs to be an object" , search.type() == Object );

            unsigned limit = 50;
            if ( cmdObj["limit"].isNumber() )
                limit = (unsigned)cmdObj["limit"].numberInt();

            si->searchCommand( d , idxNum , n.Obj() , maxDistance.numberDouble() , search.Obj() , result , limit );

            return 1;
        }

    } nameSearchCommand;





}
