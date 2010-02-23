// geo2d.cpp

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

#include "stdafx.h"
#include "namespace.h"
#include "jsobj.h"
#include "index.h"
#include "../util/unittest.h"
#include "commands.h"
#include "pdfile.h"
#include "btree.h"
#include "curop.h"
#include "matcher.h"

namespace mongo {
    
    const string GEO2DNAME = "2d";

    class GeoBitSets {
    public:
        GeoBitSets(){
            for ( int i=0; i<32; i++ ){
                masks[i] = ( 1 << ( 31 - i ) );
            }
        }
        int masks[32];
    } geoBitSets;

    
    class GeoHash {
    public:
        GeoHash()
            : _hash(""){
        }

        GeoHash( const char * s )
            : _hash( s ){
        }

        GeoHash( const string& hash )
            : _hash( hash ){
        }

        GeoHash( const BSONElement& e ){
            assert( e.type() == String );
            _hash = e.valuestr();
        }
        
        GeoHash( unsigned x , unsigned y , unsigned bits=32){
            init( x , y , bits );
        }

        void init( unsigned x , unsigned y , unsigned bits ){
            StringBuilder buf(64);
            for ( unsigned i=0; i<bits; i++ ){
                buf.append( isBitSet( x , i ) ? "1" : "0" );
                buf.append( isBitSet( y , i ) ? "1" : "0" );
            }
            _hash = buf.str();
        }

        void unhash( unsigned& x , unsigned& y ) const {
            x = 0;
            y = 0;
            size_t max = _hash.size() / 2;
            for ( size_t i=0; i<max; i++ ){
                if ( _hash[i*2] == '1' )
                    x |= geoBitSets.masks[i];
                if ( _hash[1+(i*2)] == '1' )
                    y |= geoBitSets.masks[i];
            }
        }

        /**
         * @param 0 = high
         */
        static bool isBitSet( unsigned val , unsigned  bit ){
            return geoBitSets.masks[bit] & val;
        }
        
        GeoHash up() const {
            return _hash.substr( 0 , _hash.size() - 2 );
        }
        
        bool hasPrefix( const BSONElement& e ) const {
            assert( e.type() == String );
            return strstr( e.valuestr() , _hash.c_str() ) == e.valuestr();
        }

        operator string() const { return _hash; }
        string toString() const { return _hash; }

        char operator[]( const size_t pos ) const {
            return _hash[pos];
        }

        size_t size() const {
            return _hash.size();
        }

        BSONObj wrap() const {
            return BSON( "" << _hash );
        }

        bool constrains() const {
            return _hash.size() > 0;
        }
        
        void move( int x , int y ){
            assert( _hash.size() );
            _move( 0 , x );
            _move( 1 , y );
        }

        void _move( unsigned offset , int d ){
            if ( d == 0 )
                return;
            assert( d <= 1 && d>= -1 ); // TEMP
            
            char from, to;
            if ( d > 0 ){
                from = '0';
                to = '1';
            }
            else {
                from = '1';
                to = '0';
            }

            int pos = _hash.size() - 1;
            if ( offset == 0 )
                pos--;
            for ( ; pos >= 0 ; pos-=2 ){
                if ( _hash[pos] == from ){
                    _hash[pos] = to;
                    return;
                }
                else {
                    _hash[pos] = from;
                }
            }
            
            assert(0);
        }

        void reset( const string& s ){
            _hash = s;
        }

        GeoHash& operator=(const GeoHash& h) { 
            reset(h._hash);
            return *this;
        }
        
        string _hash;
    };

    ostream& operator<<( ostream &s, const GeoHash &h ){
        s << h._hash;
        return s;
    }

    class Geo2dType : public IndexType {
    public:
        Geo2dType( const IndexPlugin * plugin , const IndexSpec* spec )
            : IndexType( plugin ) , _spec( spec ){
            
            BSONObjBuilder orderBuilder;

            BSONObjIterator i( spec->keyPattern );
            while ( i.more() ){
                BSONElement e = i.next();
                if ( e.type() == String && GEO2DNAME == e.valuestr() ){
                    uassert( 13022 , "can't have 2 geo field" , _geo.size() == 0 );
                    uassert( 13023 , "2d has to be first in index" , _other.size() == 0 );
                    _geo = e.fieldName();
                }
                else {
                    _other.push_back( e.fieldName() );
                }
                orderBuilder.append( "" , 1 );
            }
            
            uassert( 13024 , "no geo field specified" , _geo.size() );
            
            _bits = _configval( spec , "bits" , 26 ); // for lat/long, ~ 1ft
            _max = _configval( spec , "max" , 180 );
            _min = _configval( spec , "min" , -180 );
            
            _scaling = (1024*1024*1024*4.0)/(_max-_min);

            _order = orderBuilder.obj();
        }

        int _configval( const IndexSpec* spec , const string& name , int def ){
            BSONElement e = spec->info[name];
            if ( e.isNumber() )
                return e.numberInt();
            return def;
        }

        ~Geo2dType(){
            
        }

        virtual void getKeys( const BSONObj &obj, BSONObjSetDefaultOrder &keys ) const {
            BSONObjBuilder b(64);

            BSONElement geo = obj[_geo];
            uassert( 13025 , (string)"geo field[" + _geo + "] has to be an Object or Array" , geo.isABSONObj() );
            b.append( "" , _hash( geo.embeddedObject() ) );

            for ( size_t i=0; i<_other.size(); i++ ){
                BSONElement e = obj[_other[i]];
                if ( e.eoo() )
                    e = _spec->missingField();
                b.appendAs( e , "" );
            }
            keys.insert( b.obj() );
        }
        
        GeoHash _hash( const BSONObj& o ) const {
            BSONObjIterator i(o);
            assert( i.more() );
            BSONElement x = i.next();
            assert( i.more() );
            BSONElement y = i.next();
            
            uassert( 13026 , "geo values have to be numbers" , x.isNumber() && y.isNumber() );

            return _hash( x.number() , y.number() );
        }

        GeoHash _hash( double x , double y ) const {
            return GeoHash( _convert(x), _convert(y) , _bits );
        }

        BSONObj _unhash( const GeoHash& h ) const {
            unsigned x , y;
            h.unhash( x , y );
            BSONObjBuilder b;
            b.append( "x" , _unconvert( x ) );
            b.append( "y" , _unconvert( y ) );
            return b.obj();
        }
        
        unsigned _convert( double in ) const {
            uassert( 13027 , "point not in range" , in <= _max && in >= _min );
            in -= _min;
            assert( in > 0 );
            return (unsigned)(in * _scaling);
        }
        
        double _unconvert( unsigned in ) const {
            double x = in;
            x /= _scaling;
            x += _min;
            return x;
        }

        void _unconvert( const GeoHash& h , double& x , double& y ) const {
            unsigned a,b;
            h.unhash(a,b);
            x = _unconvert( a );
            y = _unconvert( b );
        }
        
        double distance( const GeoHash& a , const GeoHash& b ) const {
            double ax,ay,bx,by;
            _unconvert( a , ax , ay );
            _unconvert( b , bx , by );
            
            double dx = bx - ax;
            double dy = by - ay;

            return sqrt( ( dx * dx ) + ( dy * dy ) );
        }

        double size( const GeoHash& a ) const {
            GeoHash b = a;
            b.move( 1 , 1 );
            return distance( a , b );
        }

        const IndexSpec* _spec;
        string _geo;
        vector<string> _other;
        
        unsigned _bits;
        int _max;
        int _min;
        double _scaling;

        BSONObj _order;
    };

    class Point {
    public:
        
        Point( Geo2dType * g , const GeoHash& hash ){
            g->_unconvert( hash , _x , _y );
        }

        Point( double x , double y )
            : _x( x ) , _y( y ){
        }
        
        string toString() const {
            StringBuilder buf(32);
            buf << "(" << _x << "," << _y << ")";
            return buf.str();
  
        }

        double _x;
        double _y;
    };

    class Box {
    public:
        
        Box( Geo2dType * g , const GeoHash& hash )
            : _min( g , hash ) , 
              _max( _min._x + g->size( hash ) , _min._y + g->size( hash ) ){
        }
        
        Box( double x , double y , double size )
            : _min( x , y ) , 
              _max( x + size , y + size ){
        }

        Box( Point min , Point max )
            : _min( min ) , _max( max ){
        }

        string toString() const {
            StringBuilder buf(64);
            buf << _min.toString() << " -->> " << _max.toString();
            return buf.str();
        }


        bool between( double min , double max , double val ) const {
            return val >= min && val <= min;
        }
        
        bool mid( double amin , double amax , double bmin , double bmax , bool min , double& res ) const {
            assert( amin < amax );
            assert( bmin < bmax );

            if ( amin < bmin ){
                if ( amax < bmin )
                    return false;
                res = min ? bmin : amax;
                return true;
            }
            if ( amin > bmax )
                return false;
            res = min ? amin : bmax;
            return true;
        }

        double intersects( const Box& other ) const {
            
            Point boundMin(0,0);
            Point boundMax(0,0);
            
            if ( mid( _min._x , _max._x , other._min._x , other._max._x , true , boundMin._x ) == false ||
                 mid( _min._x , _max._x , other._min._x , other._max._x , false , boundMax._x ) == false ||
                 mid( _min._y , _max._y , other._min._y , other._max._y , true , boundMin._y ) == false ||
                 mid( _min._y , _max._y , other._min._y , other._max._y , false , boundMax._y ) == false )
                return 0;
            
            Box intersection( boundMin , boundMax );

            return intersection.area() / ( ( area() + other.area() ) / 2 );
        }

        double area() const {
            return ( _max._x - _min._x ) * ( _max._y - _min._y );
        }

        Point _min;
        Point _max;
    };
    
    class Geo2dPlugin : public IndexPlugin {
    public:
        Geo2dPlugin() : IndexPlugin( GEO2DNAME ){
        }
        
        virtual IndexType* generate( const IndexSpec* spec ) const {
            return new Geo2dType( this , spec );
        }
    } geo2dplugin;
    
    struct GeoUnitTest : public UnitTest {
        
        int round( double d ){
            return (int)(.5+(d*1000));
        }

        void run(){
            assert( ! GeoHash::isBitSet( 0 , 0 ) );
            assert( ! GeoHash::isBitSet( 0 , 31 ) );
            assert( GeoHash::isBitSet( 1 , 31 ) );
            
            IndexSpec i( BSON( "loc" << "2d" ) );
            Geo2dType g( &geo2dplugin , &i );
            {
                double x = 73.01212;
                double y = 41.352964;
                BSONObj in = BSON( "x" << x << "y" << y );
                GeoHash h = g._hash( in );
                BSONObj out = g._unhash( h );
                assert( round(x) == round( out["x"].number() ) );
                assert( round(y) == round( out["y"].number() ) );
                assert( round( in["x"].number() ) == round( out["x"].number() ) );
                assert( round( in["y"].number() ) == round( out["y"].number() ) );
            }

            {
                double x = -73.01212;
                double y = 41.352964;
                BSONObj in = BSON( "x" << x << "y" << y );
                GeoHash h = g._hash( in );
                BSONObj out = g._unhash( h );
                assert( round(x) == round( out["x"].number() ) );
                assert( round(y) == round( out["y"].number() ) );
                assert( round( in["x"].number() ) == round( out["x"].number() ) );
                assert( round( in["y"].number() ) == round( out["y"].number() ) );
            }
            
            
            {
                GeoHash h( "0000" );
                h.move( 0 , 1 );
                assert( h._hash == "0001" );
                h.move( 0 , -1 );
                assert( h._hash == "0000" );

                h.reset( "0001" );
                h.move( 0 , 1 );
                assert( h._hash == "0100" );
                h.move( 0 , -1 );
                assert( h._hash == "0001" );
                

                h.reset( "0000" );
                h.move( 1 , 0 );
                assert( h._hash == "0010" );
            }
            
            {
                Box b( 5 , 5 , 2 );
                assert( "(5,5) -->> (7,7)" == b.toString() );
            }

            {
                GeoHash a = g._hash( 1 , 1 );
                GeoHash b = g._hash( 4 , 5 );
                assert( 5 == (int)(g.distance( a , b ) ) );
            }

        }
    } geoUnitTest;
    
    class GeoPoint {
    public:
        GeoPoint( const BSONObj& o , double distance )
            : _o( o ) , _distance( distance ){
        }

        bool operator<( const GeoPoint& other ) const {
            return _distance < other._distance;
        }

        BSONObj _o;
        double _distance;
    };
    
    class GeoHopper {
    public:
        typedef multiset<GeoPoint> Holder;

        GeoHopper( Geo2dType * g , unsigned max , const GeoHash& near , const BSONObj& filter = BSONObj() )
            : _g( g ) , _max( max ) , _near( near ) , _lookedAt(0) , _objectsLoaded(0){

            if ( ! filter.isEmpty() )
                _matcher.reset( new CoveredIndexMatcher( filter , g->_spec->keyPattern ) );
        }
        
        void add( const KeyNode& node ){
            // when looking at other boxes, don't want to look at some object twice
            if ( _seen.count( node.recordLoc ) )
                return;
            _seen.insert( node.recordLoc );
            
            _lookedAt++;

            double d = _g->distance( _near , node.key.firstElement() );
            if ( _points.size() >= _max && d > farthest() )
                return;
            
            bool loaded = false;
            if ( _matcher.get() ){

                bool good = _matcher->matches( node.key , node.recordLoc , &loaded );
                if ( loaded )
                    _objectsLoaded++;
                
                if ( ! good ){
                    return;
                }
            }
            
            if ( ! loaded ) // dont double count
                _objectsLoaded++;
            
            _points.insert( GeoPoint( node.recordLoc.obj() , d ) );
            if ( _points.size() > _max ){
                _points.erase( --_points.end() );
            }
        }

        double farthest(){
            if ( _points.size() == 0 )
                return -1;
                
            Holder::iterator i = _points.end();
            i--;
            return i->_distance;
        }

        Geo2dType * _g;
        unsigned _max;
        GeoHash _near;
        Holder _points;
        set<DiskLoc> _seen;
        auto_ptr<CoveredIndexMatcher> _matcher;

        long long _lookedAt;
        long long _objectsLoaded;
    };

    struct BtreeLocation {
        int pos;
        bool found;
        DiskLoc bucket;
        
        BSONObj key(){
            if ( bucket.isNull() )
                return BSONObj();
            return bucket.btree()->keyNode( pos ).key;
        }
        
        bool hasPrefix( const GeoHash& hash ){
            BSONElement e = key().firstElement();
            if ( e.eoo() )
                return false;
            return hash.hasPrefix( e );
        }
        
        bool advance( int direction , int& totalFound , GeoHopper& all ){

            if ( bucket.isNull() )
                return false;

            bucket = bucket.btree()->advance( bucket , pos , direction , "btreelocation" );
            
            return checkCur( totalFound , all );
        }

        bool checkCur( int& totalFound , GeoHopper& all ){
            if ( bucket.isNull() )
                return false;

            if ( bucket.btree()->isUsed(pos) ){
                totalFound++;
                all.add( bucket.btree()->keyNode( pos ) );
            }

            return true;
        }
    };

    class Geo2dFindNearCmd : public Command {
    public:
        Geo2dFindNearCmd() : Command( "geo2d" ){}

        bool readOnly() { return true; }
        bool slaveOk() { return true; }
        bool slaveOverrideOk() { return true; }

        void doBox( const IndexDetails& id , Geo2dType* g , 
                    GeoHopper& hopper , long long& nscanned , int& found ,
                    const Box& want , const GeoHash& toscan , int depth = 0 ){
            Box testBox( g , toscan );

            double intPer = testBox.intersects( want );

            if ( intPer <= 0 )
                return;
            
            if ( intPer < .5 && depth < 3 ){
                doBox( id , g , hopper , nscanned , found , want , toscan._hash + "00" , depth + 1);
                doBox( id , g , hopper , nscanned , found , want , toscan._hash + "01" , depth + 1);
                doBox( id , g , hopper , nscanned , found , want , toscan._hash + "10" , depth + 1);
                doBox( id , g , hopper , nscanned , found , want , toscan._hash + "11" , depth + 1);
                return;
            }

            BtreeLocation loc;
            loc.bucket = id.head.btree()->locate( id , id.head , toscan.wrap() , g->_order , 
                                                        loc.pos , loc.found , minDiskLoc );
            loc.checkCur( found , hopper );
            while ( loc.hasPrefix( toscan ) && loc.advance( 1 , found , hopper ) )
                nscanned++;

        }

        bool run(const char * stupidns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl){
            string ns = nsToDatabase( stupidns ) + "." + cmdObj.firstElement().valuestr();

            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( ! d ){
                errmsg = "can't find ns";
                return false;
            }

            int geoIdx = -1;
            {
                NamespaceDetails::IndexIterator ii = d->ii();
                while ( ii.more() ){
                    IndexDetails& id = ii.next();
                    if ( id.getSpec().getTypeName() == GEO2DNAME ){
                        if ( geoIdx >= 0 ){
                            errmsg = "2 geo indexes :(";
                            return false;
                        }
                        geoIdx = ii.pos() - 1;
                    }
                }
            }
            
            if ( geoIdx < 0 ){
                errmsg = "no geo index :(";
                return false;
            }
            
            result.append( "ns" , ns );

            IndexDetails& id = d->idx( geoIdx );
            Geo2dType * g = (Geo2dType*)id.getSpec().getType();
            
            GeoHash n;
            {
                BSONElement nearElement = cmdObj["near"];
                if ( nearElement.isABSONObj() ){
                    n = g->_hash( cmdObj["near"].embeddedObjectUserCheck() );
                }
                else if ( nearElement.type() == String){
                    n = (string)(nearElement.valuestr());
                }
                else {
                    errmsg = "near invalid";
                    return false;
                }
            }
            result.append( "near" , n );
            
            GeoHash start = n;
            if ( cmdObj["start"].type() == String){
                start = (string)cmdObj["start"].valuestr();
                if ( 2 * ( start.size() / 2 ) != start.size() ){
                    errmsg = "start has to be an even size";
                    return false;
                }
            }

            int numWanted = 100;
            if ( cmdObj["num"].isNumber() )
                numWanted = cmdObj["num"].numberInt();
            
            long long nscanned = 0;

            BtreeBucket * head = id.head.btree();
            
            /*
             * Search algorithm
             * 1) use geohash prefix to find X items
             * 2) compute max distance from want to an item
             * 3) find optimal set of boxes that complete circle
             * 4) use regular btree cursors to scan those boxes
             */
            
            int found = 0;
            BSONObj filter;
            if ( cmdObj["query"].type() == Object )
                filter = cmdObj["query"].embeddedObject();

            GeoHopper hopper( g , numWanted , n , filter );

            GeoHash prefix = start;
            { // 1 regular geo hash algorithm
                

                BtreeLocation min;
                min.bucket = head->locate( id , id.head , n.wrap() , g->_order , min.pos , min.found , minDiskLoc );
                min.checkCur( found , hopper );
                BtreeLocation max = min;
                
                while ( found < numWanted && prefix.constrains() ){
                    while ( min.hasPrefix( prefix ) && min.advance( -1 , found , hopper ) )
                        nscanned++;
                    while ( max.hasPrefix( prefix ) && max.advance( 1 , found , hopper ) )
                        nscanned++;
                    prefix = prefix.up();
                }
            }
            
            if ( found && prefix.size() ){
                Point center( g , n );
                double boxSize = g->size( prefix );
                Box want( center._x - ( boxSize / 2 ) , center._y - ( boxSize / 2 ) , boxSize );
                
                for ( int x=-1; x<=1; x++ ){
                    for ( int y=-1; y<=1; y++ ){
                        GeoHash toscan = prefix;
                        toscan.move( x , y );
                        
                        doBox( id , g , hopper , nscanned , found , want , toscan );
                    }
                }
            }
            
            double distanceMultipier = 1;
            if ( cmdObj["distanceMultipier"].isNumber() )
                distanceMultipier = cmdObj["distanceMultipier"].number();
            
            double totalDistance = 0;

            BSONObjBuilder arr( result.subarrayStart( "results" ) );
            int x = 0;
            for ( GeoHopper::Holder::iterator i=hopper._points.begin(); i!=hopper._points.end(); i++ ){
                const GeoPoint& p = *i;
                
                double dis = distanceMultipier * p._distance;
                totalDistance += dis;
                
                BSONObjBuilder bb( arr.subobjStart( BSONObjBuilder::numStr( x++ ).c_str() ) );
                bb.append( "dis" , dis );
                bb.append( "obj" , p._o );
                bb.done();
            }
            arr.done();
            
            BSONObjBuilder stats( result.subobjStart( "stats" ) );
            stats.append( "time" , cc().curop()->elapsedMillis() );
            stats.appendIntOrLL( "btreelocs" , nscanned );
            stats.appendIntOrLL( "nscanned" , hopper._lookedAt );
            stats.appendIntOrLL( "objectsLoaded" , hopper._objectsLoaded );
            stats.append( "avgDistance" , totalDistance / x );
            stats.done();
            
            return true;
        }
        
    } geo2dFindNearCmd;
}
