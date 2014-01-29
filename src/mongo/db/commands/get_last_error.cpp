// get_last_error.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/write_concern.h"

namespace mongo {

    /* reset any errors so that getlasterror comes back clean.

       useful before performing a long series of operations where we want to
       see if any of the operations triggered an error, but don't want to check
       after each op as that woudl be a client/server turnaround.
    */
    class CmdResetError : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual void help( stringstream& help ) const {
            help << "reset error state (used with getpreverror)";
        }
        CmdResetError() : Command("resetError", false, "reseterror") {}
        bool run(const string& db, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.get();
            verify( le );
            le->reset();
            return true;
        }
    } cmdResetError;

    /* set by replica sets if specified in the configuration.
       a pointer is used to avoid any possible locking issues with lockless reading (see below locktype() is NONE
       and would like to keep that)
       (for now, it simply orphans any old copy as config changes should be extremely rare).
       note: once non-null, never goes to null again.
    */
    BSONObj *getLastErrorDefault = 0;

    class CmdGetLastError : public Command {
    public:
        CmdGetLastError() : Command("getLastError", false, "getlasterror") { }
        virtual LockType locktype() const { return NONE;  }
        virtual bool logTheOp()           { return false; }
        virtual bool slaveOk() const      { return true;  }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual void help( stringstream& help ) const {
            lastError.disableForCommand(); // SERVER-11492
            help << "return error status of the last operation on this connection\n"
                 << "options:\n"
                 << "  { fsync:true } - fsync before returning, or wait for journal commit if running with --journal\n"
                 << "  { j:true } - wait for journal commit if running with --journal\n"
                 << "  { w:n } - await replication to n servers (including self) before returning\n"
                 << "  { w:'majority' } - await replication to majority of set\n"
                 << "  { wtimeout:m} - timeout for w in m milliseconds";
        }

        bool run( const string& dbname,
                  BSONObj& cmdObj,
                  int,
                  string& errmsg,
                  BSONObjBuilder& result,
                  bool fromRepl ) {

            //
            // Correct behavior here is very finicky.
            //
            // 1.  The first step is to append the error that occurred on the previous operation.
            // This adds an "err" field to the command, which is *not* the command failing.
            //
            // 2.  Next we parse and validate write concern options.  If these options are invalid
            // the command fails no matter what, even if we actually had an error earlier.  The
            // reason for checking here is to match legacy behavior on these kind of failures -
            // we'll still get an "err" field for the write error.
            //
            // 3.  If we had an error on the previous operation, we then return immediately.
            //
            // 4.  Finally, we actually enforce the write concern.  All errors *except* timeout are
            // reported with ok : 0.0, to match legacy behavior.
            //
            // There is a special case when "wOpTime" is explicitly provided by the user - in this
            // we *only* enforce the write concern if it is valid.
            //
            // We always need to either report "err" (if ok : 1) or "errmsg" (if ok : 0), even if
            // err is null.
            //

            LastError *le = lastError.disableForCommand();

            // Always append lastOp and connectionId
            Client& c = cc();
            c.appendLastOp( result );

            // for sharding; also useful in general for debugging
            result.appendNumber( "connectionId" , c.getConnectionId() );

            bool errorOccurred = false;
            BSONObj writeConcernDoc = cmdObj;

            // Errors aren't reported when wOpTime is used
            if ( cmdObj["wOpTime"].eoo() ) {
                if ( le->nPrev != 1 ) {
                    errorOccurred = LastError::noError.appendSelf( result, false );
                    le->appendSelfStatus( result );
                }
                else {
                    errorOccurred = le->appendSelf( result, false );
                }
            }

            // Use the default options if we have no gle options aside from wOpTime
            bool useDefaultGLEOptions = cmdObj.nFields() == 1
                                        || ( cmdObj.nFields() == 2 && !cmdObj["wOpTime"].eoo() );

            if ( useDefaultGLEOptions && getLastErrorDefault ) {
                writeConcernDoc = *getLastErrorDefault;
            }

            //
            // Validate write concern no matter what, this matches 2.4 behavior
            //

            WriteConcernOptions writeConcern;
            Status status = writeConcern.parse( writeConcernDoc );

            if ( status.isOK() ) {
                // Ensure options are valid for this host
                status = validateWriteConcern( writeConcern );
            }

            if ( !status.isOK() ) {
                result.append( "badGLE", writeConcernDoc );
                return appendCommandStatus( result, status );
            }

            // Don't wait for replication if there was an error reported - this matches 2.4 behavior
            if ( errorOccurred ) {
                dassert( cmdObj["wOpTime"].eoo() );
                return true;
            }

            // No error occurred, so we won't duplicate these fields with write concern errors
            dassert( result.asTempObj()["err"].eoo() );
            dassert( result.asTempObj()["code"].eoo() );

            OpTime wOpTime;
            if ( cmdObj["wOpTime"].type() == Timestamp ) {
                // Get the wOpTime from the command if it exists
                wOpTime = OpTime( cmdObj["wOpTime"].date() );
            }
            else {
                // Use the client opTime if no wOpTime is specified
                wOpTime = cc().getLastOp();
            }

            cc().curop()->setMessage( "waiting for write concern" );

            WriteConcernResult wcResult;
            status = waitForWriteConcern( writeConcern, wOpTime, &wcResult );
            wcResult.appendTo( &result );

            // For backward compatibility with 2.4, wtimeout returns ok : 1.0
            if ( wcResult.wTimedOut ) {
                dassert( !wcResult.err.empty() ); // so we always report err
                dassert( !status.isOK() );
                result.append( "errmsg", "timed out waiting for slaves" );
                result.append( "code", status.code() );
                return true;
            }

            return appendCommandStatus( result, status );
        }

    } cmdGetLastError;

    class CmdGetPrevError : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool logTheOp() {
            return false;
        }
        virtual void help( stringstream& help ) const {
            help << "check for errors since last reseterror commandcal";
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        CmdGetPrevError() : Command("getPrevError", false, "getpreverror") {}
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.disableForCommand();
            le->appendSelf( result );
            if ( le->valid )
                result.append( "nPrev", le->nPrev );
            else
                result.append( "nPrev", -1 );
            return true;
        }
    } cmdGetPrevError;

}
