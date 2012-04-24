// lasterror.h

/*    Copyright 2009 10gen Inc.
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

#pragma once

#include "mongo/bson/oid.h"

namespace mongo {
    class BSONObjBuilder;
    class Message;

    struct LastError {
        int code;
        string msg;
        enum UpdatedExistingType { NotUpdate, True, False } updatedExisting;
        OID upsertedId;
        OID writebackId; // this shouldn't get reset so that old GLE are handled
        long long nObjects;
        int nPrev;
        bool valid;
        bool disabled;
        void writeback( OID& oid ) {
            reset( true );
            writebackId = oid;
        }
        void raiseError(int _code , const char *_msg) {
            reset( true );
            code = _code;
            msg = _msg;
        }
        void recordUpdate( bool _updateObjects , long long _nObjects , OID _upsertedId ) {
            reset( true );
            nObjects = _nObjects;
            updatedExisting = _updateObjects ? True : False;
            if ( _upsertedId.isSet() )
                upsertedId = _upsertedId;

        }
        void recordDelete( long long nDeleted ) {
            reset( true );
            nObjects = nDeleted;
        }
        LastError() {
            reset();
        }
        void reset( bool _valid = false ) {
            code = 0;
            msg.clear();
            updatedExisting = NotUpdate;
            nObjects = 0;
            nPrev = 1;
            valid = _valid;
            disabled = false;
            upsertedId.clear();
        }

        /**
         * @return if there is an err
         */
        bool appendSelf( BSONObjBuilder &b , bool blankErr = true );

        /**
         * appends fields which are not "error" related
         * this whole mechanism needs to be re-written
         * but needs a lot of real thought
         */
        void appendSelfStatus( BSONObjBuilder &b );

        struct Disabled : boost::noncopyable {
            Disabled( LastError * le ) {
                _le = le;
                if ( _le ) {
                    _prev = _le->disabled;
                    _le->disabled = true;
                }
                else {
                    _prev = false;
                }
            }

            ~Disabled() {
                if ( _le )
                    _le->disabled = _prev;
            }

            LastError * _le;
            bool _prev;
        };

        static LastError noError;
    };

    extern class LastErrorHolder {
    public:
        LastErrorHolder(){}
        ~LastErrorHolder();

        LastError * get( bool create = false );
        LastError * getSafe() {
            LastError * le = get(false);
            if ( ! le ) {
                error() << " no LastError!" << endl;
                verify( le );
            }
            return le;
        }

        LastError * _get( bool create = false ); // may return a disabled LastError

        void reset( LastError * le );

        /** ok to call more than once. */
        void initThread();

        int getID();
        
        void release();

        /** when db receives a message/request, call this */
        LastError * startRequest( Message& m , LastError * connectionOwned );

        void disconnect( int clientId );

        // used to disable lastError reporting while processing a killCursors message
        // disable causes get() to return 0.
        LastError *disableForCommand(); // only call once per command invocation!
    private:
        boost::thread_specific_ptr<LastError> _tl;

        struct Status {
            time_t time;
            LastError *lerr;
        };
    } lastError;

    void setLastError(int code , const char *msg);

} // namespace mongo
