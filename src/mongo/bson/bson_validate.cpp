// bson_validate.cpp

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/bson/bson_validate.h"
#include "mongo/bson/oid.h"

namespace mongo {

    namespace {

        class Buffer {
        public:
            Buffer( const char* buffer, uint64_t maxLength )
                : _buffer( buffer ), _position( 0 ), _maxLength( maxLength ) {
            }

            template<typename N>
            bool readNumber( N* out ) {
                if ( ( _position + sizeof(N) ) > _maxLength )
                    return false;
                if ( out ) {
                    const N* temp = reinterpret_cast<const N*>(_buffer + _position);
                    *out = *temp;
                }
                _position += sizeof(N);
                return true;
            }

            Status readCString( StringData* out ) {
                const void* x = memchr( _buffer + _position, 0, _maxLength - _position );
                if ( !x )
                    return Status( ErrorCodes::InvalidBSON, "no end of c-string" );
                uint64_t len = static_cast<uint64_t>( static_cast<const char*>(x) - ( _buffer + _position ) );

                StringData data( _buffer + _position, len );
                _position += len + 1;

                if ( out ) {
                    *out = data;
                }
                return Status::OK();
            }

            Status readUTF8String( StringData* out ) {
                int sz;
                if ( !readNumber<int>( &sz ) )
                    return Status( ErrorCodes::InvalidBSON, "invalid bson" );

                if ( out ) {
                    *out = StringData( _buffer + _position, sz );
                }

                if ( !skip( sz - 1 ) )
                    return Status( ErrorCodes::InvalidBSON, "invalid bson" );

                char c;
                if ( !readNumber<char>( &c ) )
                    return Status( ErrorCodes::InvalidBSON, "invalid bson" );

                if ( c != 0 )
                    return Status( ErrorCodes::InvalidBSON, "not null terminate string" );

                return Status::OK();
            }

            bool skip( uint64_t sz ) {
                _position += sz;
                return _position < _maxLength;
            }

            uint64_t position() const {
                return _position;
            }

        private:
            const char* _buffer;
            uint64_t _position;
            uint64_t _maxLength;
        };

        Status validateBSONInternal( Buffer* buffer, int* bsonLength ) {
            const int start = buffer->position();

            int supposedSize;
            if ( !buffer->readNumber<int>(&supposedSize) )
                return Status( ErrorCodes::InvalidBSON, "bson size is larger than buffer size" );

            Status status = Status::OK();

            while ( true ) {
                char type;
                if ( !buffer->readNumber<char>(&type) )
                    return Status( ErrorCodes::InvalidBSON, "invalid bson" );

                if ( type == EOO )
                    break;

                StringData name;
                status = buffer->readCString( &name );
                if ( !status.isOK() )
                    return status;

                switch ( type ) {
                case MinKey:
                case MaxKey:
                case jstNULL:
                case Undefined:
                    break;

                case jstOID:
                    if ( !buffer->skip( sizeof(OID) ) )
                        return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                    break;

                case NumberInt:
                    if ( !buffer->skip( sizeof(int32_t) ) )
                        return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                    break;

                case Bool:
                    if ( !buffer->skip( sizeof(int8_t) ) )
                        return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                    break;


                case NumberDouble:
                case NumberLong:
                case Timestamp:
                case Date:
                    if ( !buffer->skip( sizeof(int64_t) ) )
                        return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                    break;

                case DBRef:
                    status = buffer->readUTF8String( NULL );
                    if ( !status.isOK() )
                        return status;
                    buffer->skip( sizeof(OID) );
                    break;

                case CodeWScope: {
                    int myStart = buffer->position();
                    int sz;

                    if ( !buffer->readNumber<int>( &sz ) )
                        return Status( ErrorCodes::InvalidBSON, "invalid bson" );

                    status = buffer->readUTF8String( NULL );
                    if ( !status.isOK() )
                        return status;

                    status = validateBSONInternal( buffer, NULL );
                    if ( !status.isOK() )
                        return status;

                    if ( sz != static_cast<int>(buffer->position() - myStart) )
                        return Status( ErrorCodes::InvalidBSON, "CodeWScope len is wrong" );

                    break;
                }
                case RegEx:
                    status = buffer->readCString( NULL );
                    if ( !status.isOK() )
                        return status;
                    status = buffer->readCString( NULL );
                    if ( !status.isOK() )
                        return status;

                    break;

                case Code:
                case Symbol:
                case String:
                    status = buffer->readUTF8String( NULL );
                    if ( !status.isOK() )
                        return status;
                    break;

                case BinData: {
                    int sz;
                    if ( !buffer->readNumber<int>( &sz ) )
                        return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                    if ( !buffer->skip( 1 + sz ) )
                        return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                    break;
                }
                case Object:
                case Array:
                    status = validateBSONInternal( buffer, NULL );
                    if ( !status.isOK() )
                        return status;
                    break;

                default:
                    return Status( ErrorCodes::InvalidBSON, "invalid bson type" );
                }
            }

            const int end = buffer->position();

            if ( end - start != supposedSize )
                return Status( ErrorCodes::InvalidBSON, "bson length doesn't match what we found" );

            if ( bsonLength )
                *bsonLength = supposedSize;

            return Status::OK();
        }
    }

    Status validateBSON( const char* originalBuffer, uint64_t maxLength, int* bsonLength ) {
        if ( maxLength < 5 ) {
            return Status( ErrorCodes::InvalidBSON, "bson data has to be at least 5 bytes" );
        }

        Buffer buf( originalBuffer, maxLength );
        return validateBSONInternal( &buf, bsonLength );
    }

}
