// record.cpp

#include "pch.h"
#include "pdfile.h"
#include "../util/processinfo.h"
#include "../util/net/listen.h"
#include "pagefault.h"

namespace mongo {

    namespace ps {
        
        enum State {
            In , Out, Unk
        };

        enum Constants {
            SliceSize = 65536 , 
            MaxChain = 20 , // intentionally very low
            NumSlices = 10 ,
            RotateTimeSecs = 90 
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
        } rolling;
        
    }

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

    bool Record::likelyInPhysicalMemory() const {
        DEV if ( rand() % 100 == 0 ) return false;

        if ( ! MemoryTrackingEnabled )
            return true;

        const size_t page = (size_t)_data >> 12;
        const size_t region = page >> 6;
        const size_t offset = page & 0x3f;
        
        if ( ps::rolling.access( region , offset , false ) ) {
#ifdef _DEBUG
            if ( blockSupported && ! ProcessInfo::blockInMemory( const_cast<char*>(_data) ) ) {
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

        return ProcessInfo::blockInMemory( const_cast<char*>(_data) );
    }


    Record* Record::accessed() {
        const size_t page = (size_t)_data >> 12;
        const size_t region = page >> 6;
        const size_t offset = page & 0x3f;        
        ps::rolling.access( region , offset , true );
        return this;
    }
    
    Record* DiskLoc::rec() const {
        Record *r = DataFileMgr::getRecord(*this);
        memconcept::is(r, memconcept::concept::record);
        return r;
    }

    void Record::_accessing() const {
        if ( cc().allowedToThrowPageFaultException() && ! likelyInPhysicalMemory() ) {
            throw PageFaultException(this);
        }
    }

    void DeletedRecord::_accessing() const {

    }

}
