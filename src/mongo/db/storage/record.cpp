// record.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/storage/record.h"

#include "mongo/base/init.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_holder.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/pdfile.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/stack_introspect.h"

namespace mongo {

    RecordStats recordStats;

    void RecordStats::record( BSONObjBuilder& b ) {
        b.appendNumber( "accessesNotInMemory" , accessesNotInMemory.load() );
        b.appendNumber( "pageFaultExceptionsThrown" , pageFaultExceptionsThrown.load() );

    }

    void Record::appendStats( BSONObjBuilder& b ) {
        recordStats.record( b );
    }

    namespace ps {
        
        enum State {
            In , Out, Unk
        };

        enum Constants {
            SliceSize = 1024 , 
            MaxChain = 20 , // intentionally very low
            NumSlices = 10 ,
            RotateTimeSecs = 90 ,
            BigHashSize = 128
        };
        
        int hash( size_t region ) {
            return 
                abs( ( ( 7 + (int)(region & 0xFFFF) ) 
                       * ( 11 + (int)( ( region >> 16 ) & 0xFFFF ) ) 
#if defined(_WIN64) || defined(__amd64__)
                       * ( 13 + (int)( ( region >> 32 ) & 0xFFFF ) )
                       * ( 17 + (int)( ( region >> 48 ) & 0xFFFF ) )
#endif
                       ) % SliceSize );
        }
        
                
        /**
         * simple hash map for region -> status
         * this constitutes a single region of time
         * it does chaining, but very short chains
         */
        class Slice {
            
            struct Entry {
                size_t region;
                unsigned long long value;
            };

        public:
            
            Slice() {
                reset();
            }
            
            void reset() {
                memset( _data , 0 , SliceSize * sizeof(Entry) );
                _lastReset = time(0);
            }

            State get( int regionHash , size_t region  , short offset ) {
                DEV verify( hash( region ) == regionHash );
                
                Entry * e = _get( regionHash , region , false );
                if ( ! e )
                    return Unk;
                
                return ( e->value & ( 1ULL << offset ) ) ? In : Out;
            }
            
            /**
             * @return true if added, false if full
             */
            bool in( int regionHash , size_t region , short offset ) {
                DEV verify( hash( region ) == regionHash );
                
                Entry * e = _get( regionHash , region , true );
                if ( ! e )
                    return false;
                
                e->value |= 1ULL << offset;
                return true;
            }


            void addPages( unordered_set<size_t>* pages ) {
                for ( int i = 0; i < SliceSize; i++ ) {
                    unsigned long long v = _data[i].value;
                    
                    while ( v ) {
                        int offset = firstBitSet( v ) - 1;
                        
                        size_t page = ( _data[i].region << 6 | offset );
                        pages->insert( page );

                        v &= ~( 1ULL << offset );
                    }
                }
            }

            time_t lastReset() const { return _lastReset; }
        private:

            Entry* _get( int start , size_t region , bool add ) {
                for ( int i=0; i<MaxChain; i++ ) {

                    int bucket = ( start + i ) % SliceSize;
                    
                    if ( _data[bucket].region == 0 ) {
                        if ( ! add ) 
                            return 0;

                        _data[bucket].region = region;
                        return &_data[bucket];
                    }
                    
                    if ( _data[bucket].region == region ) {
                        return &_data[bucket];
                    }
                }
                return 0;
            }

            Entry _data[SliceSize];
            time_t _lastReset;
        };
        
        
        /**
         * this contains many slices of times
         * the idea you put mem status in the current time slice
         * and then after a certain period of time, it rolls off so we check again
         */
        class Rolling {
            
        public:
            Rolling() 
                : _lock( "ps::Rolling" ){
                _curSlice = 0;
                _lastRotate = Listener::getElapsedTimeMillis();
            }
            

            /**
             * after this call, we assume the page is in ram
             * @param doHalf if this is a known good access, want to put in first half
             * @return whether we know the page is in ram
             */
            bool access( size_t region , short offset , bool doHalf ) {
                int regionHash = hash(region);
                
                SimpleMutex::scoped_lock lk( _lock );

                static int rarely_count = 0;
                if ( rarely_count++ % ( 2048 / BigHashSize ) == 0 ) {
                    long long now = Listener::getElapsedTimeMillis();
                    RARELY if ( now == 0 ) {
                        MONGO_TLOG(0) << "warning Listener::getElapsedTimeMillis returning 0ms" << endl;
                    }
                    
                    if ( now - _lastRotate > ( 1000 * RotateTimeSecs ) ) {
                        _rotate();
                    }
                }
                
                for ( int i=0; i<NumSlices / ( doHalf ? 2 : 1 ); i++ ) {
                    int pos = (_curSlice+i)%NumSlices;
                    State s = _slices[pos].get( regionHash , region , offset );

                    if ( s == In )
                        return true;
                    
                    if ( s == Out ) {
                        _slices[pos].in( regionHash , region , offset );
                        return false;
                    }
                }

                // we weren't in any slice
                // so add to cur
                if ( ! _slices[_curSlice].in( regionHash , region , offset ) ) {
                    _rotate();
                    _slices[_curSlice].in( regionHash , region , offset );
                }
                return false;
            }

            /**
             * @param pages OUT adds each page to the set
             * @param mySlices temporary space for copy
             * @return the oldest timestamp we have
             */
            time_t addPages( unordered_set<size_t>* pages, Slice* mySlices ) {
                time_t oldestTimestamp = std::numeric_limits<time_t>::max();
                {
                    // by doing this, we're in the lock only about half as long as the naive way
                    // that's measure with a small data set
                    // Assumption is that with a large data set, actually adding to set may get more costly
                    // so this way the time in lock should be totally constant
                    SimpleMutex::scoped_lock lk( _lock );
                    memcpy( mySlices, _slices, NumSlices * sizeof(Slice) );

                    for ( int i = 0; i < NumSlices; i++ ) {
                        oldestTimestamp = std::min( oldestTimestamp, _slices[i].lastReset() );
                    }
                }

                for ( int i = 0; i < NumSlices; i++ ) {
                    mySlices[i].addPages( pages );
                }

                return oldestTimestamp;
            }
        private:
            
            void _rotate() {
                _curSlice = ( _curSlice + 1 ) % NumSlices;
                _slices[_curSlice].reset();
                _lastRotate = Listener::getElapsedTimeMillis();
            }

            int _curSlice;
            long long _lastRotate;
            Slice _slices[NumSlices];

            SimpleMutex _lock;
        };

        Rolling* rolling = new Rolling[BigHashSize];
        
        int bigHash( size_t region ) {
            return hash( region ) % BigHashSize;
        }

        namespace PointerTable {

            /* A "superpage" is a group of 16 contiguous pages that differ
             * only in the low-order 16 bits. This means that there is
             * enough room in the low-order bits to store a bitmap for each
             * page in the superpage.
             */
            static const size_t superpageMask = ~0xffffLL;
            static const size_t superpageShift = 16;
            static const size_t pageSelectorMask = 0xf000LL; // selects a page in a superpage
            static const int pageSelectorShift = 12;
                
            // Tunables
            static const int capacity = 128; // in superpages
            static const int bucketSize = 4; // half cache line
            static const int buckets = capacity/bucketSize;
            
            struct Data {
                /** organized similar to a CPU cache
                 *  bucketSize-way set associative
                 *  least-recently-inserted replacement policy
                 */
                size_t _table[buckets][bucketSize];
                long long _lastReset; // time in millis
            };

            void reset(Data* data) {
                memset(data->_table, 0, sizeof(data->_table));
                data->_lastReset = Listener::getElapsedTimeMillis();
            }

            inline void resetIfNeeded( Data* data ) {
                const long long now = Listener::getElapsedTimeMillis();
                if (MONGO_unlikely(now - data->_lastReset > RotateTimeSecs*1000))
                    reset(data);
            }

            inline size_t pageBitOf(size_t ptr) {
                return 1LL << ((ptr & pageSelectorMask) >> pageSelectorShift);
            }
            
            inline size_t superpageOf(size_t ptr) {
                return ptr & superpageMask;
            }

            inline size_t bucketFor(size_t ptr) {
                return (ptr >> superpageShift) % buckets;
            }

            inline bool haveSeenPage(size_t superpage, size_t ptr) {
                return superpage & pageBitOf(ptr);
            }

            inline void markPageSeen(size_t& superpage, size_t ptr) {
                superpage |= pageBitOf(ptr);
            }

            /** call this to check a page has been seen yet. */
            inline bool seen(Data* data, size_t ptr) {
                resetIfNeeded(data);

                // A bucket contains 4 superpages each containing 16 contiguous pages
                // See below for a more detailed explanation of superpages
                size_t* bucket = data->_table[bucketFor(ptr)];

                for (int i = 0; i < bucketSize; i++) {
                    if (superpageOf(ptr) == superpageOf(bucket[i])) {
                        if (haveSeenPage(bucket[i], ptr))
                            return true;

                        markPageSeen(bucket[i], ptr);
                        return false;
                    }
                }

                // superpage isn't in thread-local cache
                // slide bucket forward and add new superpage at front
                for (int i = bucketSize-1; i > 0; i--)
                    bucket[i] = bucket[i-1];

                bucket[0] = superpageOf(ptr);
                markPageSeen(bucket[0], ptr);

                return false;
            }

            Data* getData();

        };
     
        void appendWorkingSetInfo( BSONObjBuilder& b ) {
            boost::scoped_array<Slice> mySlices( new Slice[NumSlices] );

            unordered_set<size_t> totalPages;
            Timer t;

            time_t timestamp = 0;

            for ( int i = 0; i < BigHashSize; i++ ) {
                time_t myOldestTimestamp = rolling[i].addPages( &totalPages, mySlices.get() );
                timestamp = std::max( timestamp, myOldestTimestamp );
            }

            b.append( "note", "thisIsAnEstimate" );
            b.appendNumber( "pagesInMemory", totalPages.size() );
            b.appendNumber( "computationTimeMicros", static_cast<long long>(t.micros()) );
            b.append( "overSeconds", static_cast<int>( time(0) - timestamp ) );

        }
        
    }

    
    // These need to be outside the ps namespace due to the way they are defined
#if defined(__linux__) && defined(__GNUC__)
    __thread ps::PointerTable::Data _pointerTableData;
    ps::PointerTable::Data* ps::PointerTable::getData() { 
        return &_pointerTableData; 
    }
#elif defined(_WIN32)
    __declspec( thread ) ps::PointerTable::Data _pointerTableData;
    ps::PointerTable::Data* ps::PointerTable::getData() { 
        return &_pointerTableData; 
    }
#else
    TSP_DEFINE(ps::PointerTable::Data, _pointerTableData);
    ps::PointerTable::Data* ps::PointerTable::getData() { 
        return _pointerTableData.getMake();
    }
#endif

    bool Record::MemoryTrackingEnabled = true;
    
    volatile int __record_touch_dummy = 1; // this is used to make sure the compiler doesn't get too smart on us
    void Record::touch( bool entireRecrd ) const {
        if ( _lengthWithHeaders > HeaderSize ) { // this also makes sure lengthWithHeaders is in memory
            const char * addr = _data;
            const char * end = _data + _netLength();
            for ( ; addr <= end ; addr += 2048 ) {
                __record_touch_dummy += addr[0];

                break; // TODO: remove this, pending SERVER-3711
                
                // note if this is a touch of a deletedrecord, we don't want to touch more than the first part. we may simply
                // be updated the linked list and a deletedrecord could be gigantic.  similar circumstance just less extreme 
                // exists for any record if we are just updating its header, say on a remove(); some sort of hints might be 
                // useful.

                if ( ! entireRecrd )
                    break;
            }
        }
    }

    static bool blockSupported = false;

    MONGO_INITIALIZER_WITH_PREREQUISITES(RecordBlockSupported,
                                         ("SystemInfo"))(InitializerContext* cx) {
        blockSupported = ProcessInfo::blockCheckSupported();
        return Status::OK();
    }

    void Record::appendWorkingSetInfo( BSONObjBuilder& b ) {
        if ( ! blockSupported ) {
            b.append( "info", "not supported" );
            return;
        }
        
        ps::appendWorkingSetInfo( b );
    }

    bool Record::likelyInPhysicalMemory() const {
        return likelyInPhysicalMemory( _data );
    }

    bool Record::likelyInPhysicalMemory( const char* data ) {
        DEV {
            // we don't want to do this too often as it makes DEBUG builds very slow
            // at some point we might want to pass in what type of Record this is and
            // then we can use that to make a more intelligent decision
            int mod;
            if ( Lock::isReadLocked() ) {
                // we'll check read locks less often
                // since its a lower probability of error
                mod = 1000;
            }
            else if ( Lock::isLocked() ) {
                // write lock's can more obviously cause issues
                // check more often than reads
                mod = 100;
            }
            else {
                // no lock???
                // if we get here we should be very paranoid
                mod = 50;
            }
            
            if ( rand() % mod == 0 ) 
                return false;
        } // end DEV test code

        if ( ! MemoryTrackingEnabled )
            return true;

        const size_t page = (size_t)data >> 12;
        const size_t region = page >> 6;
        const size_t offset = page & 0x3f;

        const bool seen = ps::PointerTable::seen( ps::PointerTable::getData(), reinterpret_cast<size_t>(data));
        if (seen || ps::rolling[ps::bigHash(region)].access( region , offset , false ) ) {
        
#ifdef _DEBUG
            if ( blockSupported && ! ProcessInfo::blockInMemory(data) ) {
                RARELY warning() << "we think data is in ram but system says no"  << endl;
            }
#endif
            return true;
        }

        if ( ! blockSupported ) {
            // this means we don't fallback to system call 
            // and assume things aren't in memory
            // possible we yield too much - but better than not yielding through a fault
            return false;
        }

        return ProcessInfo::blockInMemory( const_cast<char*>(data) );
    }


    Record* Record::accessed() {
        const bool seen = ps::PointerTable::seen( ps::PointerTable::getData(), reinterpret_cast<size_t>(_data));
        if (!seen){
            const size_t page = (size_t)_data >> 12;
            const size_t region = page >> 6;
            const size_t offset = page & 0x3f;        
            ps::rolling[ps::bigHash(region)].access( region , offset , true );
        }

        return this;
    }
    
    Record* DiskLoc::rec() const {
        Record *r = DataFileMgr::getRecord(*this);
        memconcept::is(r, memconcept::concept::record);
        return r;
    }

    void Record::_accessing() const {
        if ( likelyInPhysicalMemory() )
            return;

        const Client& client = cc();
        Database* db = client.database();
        
        recordStats.accessesNotInMemory.fetchAndAdd(1);
        if ( db )
            db->recordStats().accessesNotInMemory.fetchAndAdd(1);
        
        if ( ! client.allowedToThrowPageFaultException() )
            return;
        
        if ( client.curop() && client.curop()->elapsedMillis() > 50 ) {
            // this means we've been going too long to restart
            // we should track how often this happens
            return;
        }

        recordStats.pageFaultExceptionsThrown.fetchAndAdd(1);
        if ( db )
            db->recordStats().pageFaultExceptionsThrown.fetchAndAdd(1);

        DEV fassert( 16236 , ! inConstructorChain(true) );
        throw PageFaultException(this);
    }

    void DeletedRecord::_accessing() const {

    }

    namespace {
        
        class WorkingSetSSS : public ServerStatusSection {
        public:
            WorkingSetSSS() : ServerStatusSection( "workingSet" ){}
            virtual bool includeByDefault() const { return false; }
            
            BSONObj generateSection(const BSONElement& configElement) const {
                BSONObjBuilder b;
                Record::appendWorkingSetInfo( b );
                return b.obj();
            }
                
        } asserts;

        class RecordStats : public ServerStatusSection {
        public:
            RecordStats() : ServerStatusSection( "recordStats" ){}
            virtual bool includeByDefault() const { return true; }
            
            BSONObj generateSection(const BSONElement& configElement) const {
                BSONObjBuilder record;
                
                Record::appendStats( record );

                set<string> dbs;
                {
                    Lock::DBRead read( "local" );
                    dbHolder().getAllShortNames( dbs );
                }

                for ( set<string>::iterator i = dbs.begin(); i != dbs.end(); ++i ) {
                    string db = *i;
                    Client::ReadContext ctx( db );
                    BSONObjBuilder temp( record.subobjStart( db ) );
                    ctx.ctx().db()->recordStats().record( temp );
                    temp.done();
                }

                return record.obj();
            }
                
        } recordStats;

    }
}
