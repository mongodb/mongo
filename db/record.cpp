// record.cpp

#include "../pch.h"
#include "pdfile.h"
#include "../util/processinfo.h"
#include "../util/message.h"

namespace mongo {
    
    namespace ps {

        enum State {
            IN , OUT , UNK
        };

        enum Constants {
            SliceSize = 65536 , 
            MaxChain = 20 , // intentionally very low
            NumSlices = 10 ,
            RotateTimeSecs = 90 
        };
        
        int hash( size_t region ) {
            return 
                abs( ( ( 7 + (int)(region & 0xFFFF) ) * 
                       ( 11 + (int)( ( region >> 16 ) & 0xFFFF ) ) *
                       ( 13 + (int)( ( region >> 32 ) & 0xFFFF ) ) *
                       ( 17 + (int)( ( region >> 48 ) & 0xFFFF ) ) ) % SliceSize );
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

            State get( size_t region  , short offset ) {
                Entry * e = _get( region , false );
                if ( ! e )
                    return UNK;
                
                return ( e->value & ( 1 << offset ) ) ? IN : OUT;
            }
            
            /**
             * @return true if added, false if full
             */
            bool in( size_t region , short offset ) {
                Entry * e = _get( region , true );
                if ( ! e )
                    return false;
                
                e->value |= ( 1 << offset );
                return true;
            }

        private:

            Entry* _get( size_t region , bool add ) {
                int start = hash( region );
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
            Rolling() {
                _curSlice = 0;
                _lastRotate = Listener::getElapsedTimeMillis();
            }
            

            /**
             * after this call, we assume the page is in ram
             * @return whether we know the page is in ram
             */
            bool access( size_t region , short offset ) {
                scoped_spinlock lk( _lock );

                RARELY {
                    long long now = Listener::getElapsedTimeMillis();
                    OCCASIONALLY if ( now == 0 ) {
                        warning() << "Listener::getElapsedTimeMillis returing 0" << endl;
                    }
                    
                    if ( now - _lastRotate > ( 1000 * RotateTimeSecs ) ) {
                        _rotate();
                    }
                }

                for ( int i=0; i<NumSlices; i++ ) {
                    int pos = (_curSlice+i)%NumSlices;
                    State s = _slices[pos].get( region , offset );

                    if ( s == IN )
                        return true;
                    
                    if ( s == OUT ) {
                        _slices[pos].in( region , offset );
                        return false;
                    }
                }

                // we weren't in any slice
                // so add to cur
                if ( ! _slices[_curSlice].in( region , offset ) ) {
                    _rotate();
                    _slices[_curSlice].in( region , offset );
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

            SpinLock _lock;
        } rolling;
        
    }

    

    int __record_touch_dummy = 1; // this is used to make sure the compiler doesn't get too smart on us
    void Record::touch( bool entireRecrd ) {

        if ( lengthWithHeaders > HeaderSize ) { // this also makes sure lengthWithHeaders is in memory
            char * addr = data;
            char * end = data + netLength();
            for ( ; addr <= end ; addr += 2048 )
                __record_touch_dummy += addr[0];
        }

    }

    bool Record::likelyInPhysicalMemory() {
        static bool blockSupported = ProcessInfo::blockCheckSupported();

        const size_t page = (size_t)data >> 12;
        const size_t region = page >> 6;
        const size_t offset = page & 0x3f;
        
        if ( ps::rolling.access( region , offset ) )
            return true;

        if ( ! blockSupported )
            return false;
        return ProcessInfo::blockInMemory( data );
    }

    
}
