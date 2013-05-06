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

#include <deque>

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

        struct ValidationState {
            enum State {
                BeginObj = 1,
                WithinObj,
                EndObj,
                BeginCodeWScope,
                EndCodeWScope,
                Done
            };
        };

        class ValidationObjectFrame {
        public:
            int startPosition() const { return _startPosition & ~(1 << 31); }
            bool isCodeWithScope() const { return _startPosition & (1 << 31); }

            void setStartPosition(int pos) {
                _startPosition = (_startPosition & (1 << 31)) | (pos & ~(1 << 31));
            }
            void setIsCodeWithScope(bool isCodeWithScope) {
                if (isCodeWithScope) {
                    _startPosition |= 1 << 31;
                }
                else {
                    _startPosition &= ~(1 << 31);
                }
            }

            int expectedSize;
        private:
            int _startPosition;
        };

        Status validateElementInfo(Buffer* buffer, ValidationState::State* nextState) {
            Status status = Status::OK();

            signed char type;
            if ( !buffer->readNumber<signed char>(&type) )
                return Status( ErrorCodes::InvalidBSON, "invalid bson" );

            if ( type == EOO ) {
                *nextState = ValidationState::EndObj;
                return Status::OK();
            }

            StringData name;
            status = buffer->readCString( &name );
            if ( !status.isOK() )
                return status;

            switch ( type ) {
            case MinKey:
            case MaxKey:
            case jstNULL:
            case Undefined:
                return Status::OK();

            case jstOID:
                if ( !buffer->skip( sizeof(OID) ) )
                    return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                return Status::OK();

            case NumberInt:
                if ( !buffer->skip( sizeof(int32_t) ) )
                    return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                return Status::OK();

            case Bool:
                if ( !buffer->skip( sizeof(int8_t) ) )
                    return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                return Status::OK();


            case NumberDouble:
            case NumberLong:
            case Timestamp:
            case Date:
                if ( !buffer->skip( sizeof(int64_t) ) )
                    return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                return Status::OK();

            case DBRef:
                status = buffer->readUTF8String( NULL );
                if ( !status.isOK() )
                    return status;
                buffer->skip( sizeof(OID) );
                return Status::OK();

            case RegEx:
                status = buffer->readCString( NULL );
                if ( !status.isOK() )
                    return status;
                status = buffer->readCString( NULL );
                if ( !status.isOK() )
                    return status;

                return Status::OK();

            case Code:
            case Symbol:
            case String:
                status = buffer->readUTF8String( NULL );
                if ( !status.isOK() )
                    return status;
                return Status::OK();

            case BinData: {
                int sz;
                if ( !buffer->readNumber<int>( &sz ) )
                    return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                if ( !buffer->skip( 1 + sz ) )
                    return Status( ErrorCodes::InvalidBSON, "invalid bson" );
                return Status::OK();
            }
            case CodeWScope:
                *nextState = ValidationState::BeginCodeWScope;
                return Status::OK();
            case Object:
            case Array:
                *nextState = ValidationState::BeginObj;
                return Status::OK();

            default:
                return Status( ErrorCodes::InvalidBSON, "invalid bson type" );
            }
        }

        Status validateBSONIterative(Buffer* buffer) {
            std::deque<ValidationObjectFrame> frames;
            ValidationObjectFrame* curr = NULL;
            ValidationState::State state = ValidationState::BeginObj;

            while (state != ValidationState::Done) {
                switch (state) {
                case ValidationState::BeginObj:
                    frames.push_back(ValidationObjectFrame());
                    curr = &frames.back();
                    curr->setStartPosition(buffer->position());
                    curr->setIsCodeWithScope(false);
                    if (!buffer->readNumber<int>(&curr->expectedSize)) {
                        return Status(ErrorCodes::InvalidBSON,
                                      "bson size is larger than buffer size");
                    }
                    state = ValidationState::WithinObj;
                    // fall through
                case ValidationState::WithinObj: {
                    Status status = validateElementInfo(buffer, &state);
                    if (!status.isOK())
                        return status;
                    break;
                }
                case ValidationState::EndObj: {
                    int actualLength = buffer->position() - curr->startPosition();
                    if ( actualLength != curr->expectedSize ) {
                        return Status( ErrorCodes::InvalidBSON,
                                       "bson length doesn't match what we found" );
                    }
                    frames.pop_back();
                    if (frames.empty()) {
                        state = ValidationState::Done;
                    }
                    else {
                        curr = &frames.back();
                        if (curr->isCodeWithScope())
                            state = ValidationState::EndCodeWScope;
                        else
                            state = ValidationState::WithinObj;
                    }
                    break;
                }
                case ValidationState::BeginCodeWScope: {
                    frames.push_back(ValidationObjectFrame());
                    curr = &frames.back();
                    curr->setStartPosition(buffer->position());
                    curr->setIsCodeWithScope(true);
                    if ( !buffer->readNumber<int>( &curr->expectedSize ) )
                        return Status( ErrorCodes::InvalidBSON, "invalid bson CodeWScope size" );
                    Status status = buffer->readUTF8String( NULL );
                    if ( !status.isOK() )
                        return status;
                    state = ValidationState::BeginObj;
                    break;
                }
                case ValidationState::EndCodeWScope: {
                    int actualLength = buffer->position() - curr->startPosition();
                    if ( actualLength != curr->expectedSize ) {
                        return Status( ErrorCodes::InvalidBSON,
                                       "bson length for CodeWScope doesn't match what we found" );
                    }
                    frames.pop_back();
                    if (frames.empty())
                        return Status(ErrorCodes::InvalidBSON, "unnested CodeWScope");
                    curr = &frames.back();
                    state = ValidationState::WithinObj;
                    break;
                }
                case ValidationState::Done:
                    break;
                }
            }

            return Status::OK();
        }

    }  // namespace

    Status validateBSON( const char* originalBuffer, uint64_t maxLength ) {
        if ( maxLength < 5 ) {
            return Status( ErrorCodes::InvalidBSON, "bson data has to be at least 5 bytes" );
        }

        Buffer buf( originalBuffer, maxLength );
        return validateBSONIterative( &buf );
    }

}  // namespace mongo
