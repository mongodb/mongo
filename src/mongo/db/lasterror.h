// lasterror.h

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/thread/tss.hpp>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/bson/oid.h"
#include "mongo/util/log.h"

namespace mongo {
    class BSONObjBuilder;
    class Message;

    static const char kUpsertedFieldName[] = "upserted";
    static const char kGLEStatsFieldName[] = "$gleStats";
    static const char kGLEStatsLastOpTimeFieldName[] = "lastOpTime";
    static const char kGLEStatsElectionIdFieldName[] = "electionId";

    struct LastError {
        int code;
        std::string msg;
        enum UpdatedExistingType { NotUpdate, True, False } updatedExisting;
        // _id field value from inserted doc, returned as kUpsertedFieldName (above)
        BSONObj upsertedId;
        OID writebackId; // this shouldn't get reset so that old GLE are handled
        int writebackSince;
        long long nObjects;
        int nPrev;
        bool valid;
        bool disabled;
        void writeback(const OID& oid) {
            reset( true );
            writebackId = oid;
            writebackSince = 0;
        }
        void raiseError(int _code , const char *_msg) {
            reset( true );
            code = _code;
            msg = _msg;
        }
        void recordUpdate( bool _updateObjects , long long _nObjects , BSONObj _upsertedId ) {
            reset( true );
            nObjects = _nObjects;
            updatedExisting = _updateObjects ? True : False;
            if ( _upsertedId.valid() && _upsertedId.hasField(kUpsertedFieldName) )
                upsertedId = _upsertedId;

        }
        void recordDelete( long long nDeleted ) {
            reset( true );
            nObjects = nDeleted;
        }
        LastError() {
            reset();
            writebackSince = 0;
        }
        void reset( bool _valid = false ) {
            code = 0;
            msg.clear();
            updatedExisting = NotUpdate;
            nObjects = 0;
            nPrev = 1;
            valid = _valid;
            disabled = false;
            upsertedId = BSONObj();
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
                error() << " no LastError!" << std::endl;
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
