// record.cpp

#include "pch.h"
#include "mongo/db/curop.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/record.h"
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
         * this constitures a single region of time
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
            }

            State get( int regionHash , size_t region  , short offset ) {
                DEV verify( hash( region ) == regionHash );
                
                Entry * e = _get( regionHash , region , false );
                if ( ! e )
                    return Unk;
                
                return ( e->value & ( ((unsigned long long)1) << offset ) ) ? In : Out;
            }
            
            /**
             * @return true if added, false if full
             */
            bool in( int regionHash , size_t region , short offset ) {
                DEV verify( hash( region ) == regionHash );
                
                Entry * e = _get( regionHash , region , true );
                if ( ! e )
                    return false;
                
                e->value |= ((unsigned long long)1) << offset;
                return true;
            }

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
                if ( rarely_count++ % 2048 == 0 ) {
                    long long now = Listener::getElapsedTimeMillis();
                    RARELY if ( now == 0 ) {
                        tlog() << "warning Listener::getElapsedTimeMillis returning 0ms" << endl;
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

    const bool blockSupported = ProcessInfo::blockCheckSupported();

    bool Record::blockCheckSupported() { 
        return ProcessInfo::blockCheckSupported();
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
            if ( blockSupported && ! ProcessInfo::blockInMemory( const_cast<char*>(data) ) ) {
                warning() << "we think data is in ram but system says no"  << endl;
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

}
