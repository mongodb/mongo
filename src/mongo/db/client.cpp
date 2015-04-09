// client.cpp

/**
*    Copyright (C) 2009 10gen Inc.
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

/* Client represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage_options.h"
#include "mongo/s/d_state.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exit.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using std::string;
    using std::stringstream;

    using logger::LogComponent;

    boost::mutex Client::clientsMutex;
    ClientSet Client::clients;

    TSP_DEFINE(Client, currentClient)

    /**
     * This must be called whenever a new thread is started, so that active threads can be tracked
     * so each thread has a Client object in TLS.
     */
    void Client::initThread(const char *desc, AbstractMessagingPort *mp) {
        invariant(currentClient.get() == 0);

        string fullDesc;
        if (mp != NULL) {
            fullDesc = str::stream() << desc << mp->connectionId();
        }
        else {
            fullDesc = desc;
        }

        setThreadName(fullDesc.c_str());
        mongo::lastError.initThread();

        // Create the client obj, attach to thread
        Client* client = new Client(fullDesc, getGlobalServiceContext(), mp);
        client->setAuthorizationSession(
                getGlobalAuthorizationManager()->makeAuthorizationSession());

        currentClient.reset(client);

        // This makes the client visible to maintenance threads
        boost::lock_guard<boost::mutex> clientLock(clientsMutex);
        clients.insert(client);
    }

    Client::Client(const string& desc, ServiceContext* serviceContext, AbstractMessagingPort *p)
        : ClientBasic(serviceContext, p),
          _desc(desc),
          _threadId(boost::this_thread::get_id()),
          _connectionId(p ? p->connectionId() : 0),
          _inDirectClient(false),
          _txn(NULL) {
    }

    Client::~Client() {
        if ( ! inShutdown() ) {
            // we can't clean up safely once we're in shutdown
            {
                boost::lock_guard<boost::mutex> clientLock(clientsMutex);
                clients.erase(this);
            }
        }
    }

    bool Client::shutdown() {
        if (!inShutdown()) {
            boost::lock_guard<boost::mutex> clientLock(clientsMutex);
            clients.erase(this);
        }
        return false;
    }

    void Client::reportState(BSONObjBuilder& builder) {
        builder.append("desc", desc());

        std::stringstream ss;
        ss << _threadId;
        builder.append("threadId", ss.str());

        if (_connectionId) {
            builder.appendNumber("connectionId", _connectionId);
        }
    }

    void Client::setOperationContext(OperationContext* txn) {
        // We can only set the OperationContext once before resetting it.
        invariant(txn != NULL && _txn == NULL);

        boost::unique_lock<SpinLock> uniqueLock(_lock);
        _txn = txn;
    }

    void Client::resetOperationContext() {
        invariant(_txn != NULL);
        boost::unique_lock<SpinLock> uniqueLock(_lock);
        _txn = NULL;
    }

    string Client::clientAddress(bool includePort) const {
        if (!hasRemote()) {
            return "";
        }
        if (includePort) {
            return getRemote().toString();
        }
        return getRemote().host();
    }

    ClientBasic* ClientBasic::getCurrent() {
        return currentClient.get();
    }


    void OpDebug::reset() {
        extra.reset();

        op = 0;
        iscommand = false;
        ns = "";
        query = BSONObj();
        updateobj = BSONObj();

        cursorid = -1;
        ntoreturn = -1;
        ntoskip = -1;
        exhaust = false;

        nscanned = -1;
        nscannedObjects = -1;
        idhack = false;
        scanAndOrder = false;
        nMatched = -1;
        nModified = -1;
        ninserted = -1;
        ndeleted = -1;
        nmoved = -1;
        fastmod = false;
        fastmodinsert = false;
        upsert = false;
        cursorExhausted = false;
        keyUpdates = 0;  // unsigned, so -1 not possible
        writeConflicts = 0;
        planSummary = "";
        execStats.reset();
        
        exceptionInfo.reset();
        
        executionTime = 0;
        nreturned = -1;
        responseLength = -1;
    }


#define OPDEBUG_TOSTRING_HELP(x) if( x >= 0 ) s << " " #x ":" << (x)
#define OPDEBUG_TOSTRING_HELP_BOOL(x) if( x ) s << " " #x ":" << (x)
    string OpDebug::report(const CurOp& curop, const SingleThreadedLockStats& lockStats) const {
        StringBuilder s;
        if ( iscommand )
            s << "command ";
        else
            s << opToString( op ) << ' ';
        s << ns.toString();

        if ( ! query.isEmpty() ) {
            if ( iscommand ) {
                s << " command: ";
                
                Command* curCommand = curop.getCommand();
                if (curCommand) {
                    mutablebson::Document cmdToLog(query, mutablebson::Document::kInPlaceDisabled);
                    curCommand->redactForLogging(&cmdToLog);
                    s << curCommand->name << " ";
                    s << cmdToLog.toString();
                } 
                else { // Should not happen but we need to handle curCommand == NULL gracefully
                    s << query.toString();
                }
            }
            else {
                s << " query: ";
                s << query.toString();
            }
        }

        if (!planSummary.empty()) {
            s << " planSummary: " << planSummary.toString();
        }
        
        if ( ! updateobj.isEmpty() ) {
            s << " update: ";
            updateobj.toString( s );
        }

        OPDEBUG_TOSTRING_HELP( cursorid );
        OPDEBUG_TOSTRING_HELP( ntoreturn );
        OPDEBUG_TOSTRING_HELP( ntoskip );
        OPDEBUG_TOSTRING_HELP_BOOL( exhaust );

        OPDEBUG_TOSTRING_HELP( nscanned );
        OPDEBUG_TOSTRING_HELP( nscannedObjects );
        OPDEBUG_TOSTRING_HELP_BOOL( idhack );
        OPDEBUG_TOSTRING_HELP_BOOL( scanAndOrder );
        OPDEBUG_TOSTRING_HELP( nmoved );
        OPDEBUG_TOSTRING_HELP( nMatched );
        OPDEBUG_TOSTRING_HELP( nModified );
        OPDEBUG_TOSTRING_HELP( ninserted );
        OPDEBUG_TOSTRING_HELP( ndeleted );
        OPDEBUG_TOSTRING_HELP_BOOL( fastmod );
        OPDEBUG_TOSTRING_HELP_BOOL( fastmodinsert );
        OPDEBUG_TOSTRING_HELP_BOOL( upsert );
        OPDEBUG_TOSTRING_HELP_BOOL( cursorExhausted );
        OPDEBUG_TOSTRING_HELP( keyUpdates );
        OPDEBUG_TOSTRING_HELP( writeConflicts );
        
        if ( extra.len() )
            s << " " << extra.str();

        if ( ! exceptionInfo.empty() ) {
            s << " exception: " << exceptionInfo.msg;
            if ( exceptionInfo.code )
                s << " code:" << exceptionInfo.code;
        }

        s << " numYields:" << curop.numYields();
        
        OPDEBUG_TOSTRING_HELP( nreturned );
        if (responseLength > 0) {
            s << " reslen:" << responseLength;
        }

        {
            BSONObjBuilder locks;
            lockStats.report(&locks);
            s << " locks:" << locks.obj().toString();
        }

        s << " " << executionTime << "ms";
        
        return s.str();
    }

    namespace {
        /**
         * Appends {name: obj} to the provided builder.  If obj is greater than maxSize, appends a
         * string summary of obj instead of the object itself.
         */
        void appendAsObjOrString(StringData name,
                                 const BSONObj& obj,
                                 size_t maxSize,
                                 BSONObjBuilder* builder) {
            if (static_cast<size_t>(obj.objsize()) <= maxSize) {
                builder->append(name, obj);
            }
            else {
                // Generate an abbreviated serialization for the object, by passing false as the
                // "full" argument to obj.toString().
                const bool isArray = false;
                const bool full = false;
                std::string objToString = obj.toString(isArray, full);
                if (objToString.size() <= maxSize) {
                    builder->append(name, objToString);
                }
                else {
                    // objToString is still too long, so we append to the builder a truncated form
                    // of objToString concatenated with "...".  Instead of creating a new string
                    // temporary, mutate objToString to do this (we know that we can mutate
                    // characters in objToString up to and including objToString[maxSize]).
                    objToString[maxSize - 3] = '.';
                    objToString[maxSize - 2] = '.';
                    objToString[maxSize - 1] = '.';
                    builder->append(name, StringData(objToString).substr(0, maxSize));
                }
            }
        }
    } // namespace

#define OPDEBUG_APPEND_NUMBER(x) if( x != -1 ) b.appendNumber( #x , (x) )
#define OPDEBUG_APPEND_BOOL(x) if( x ) b.appendBool( #x , (x) )
    void OpDebug::append(const CurOp& curop,
                         const SingleThreadedLockStats& lockStats,
                         BSONObjBuilder& b) const {

        const size_t maxElementSize = 50 * 1024;

        b.append( "op" , iscommand ? "command" : opToString( op ) );
        b.append( "ns" , ns.toString() );

        if (!query.isEmpty()) {
            appendAsObjOrString(iscommand ? "command" : "query", query, maxElementSize, &b);
        }
        else if (!iscommand && curop.haveQuery()) {
            appendAsObjOrString("query", curop.query(), maxElementSize, &b);
        }

        if (!updateobj.isEmpty()) {
            appendAsObjOrString("updateobj", updateobj, maxElementSize, &b);
        }

        const bool moved = (nmoved >= 1);

        OPDEBUG_APPEND_NUMBER( cursorid );
        OPDEBUG_APPEND_NUMBER( ntoreturn );
        OPDEBUG_APPEND_NUMBER( ntoskip );
        OPDEBUG_APPEND_BOOL( exhaust );

        OPDEBUG_APPEND_NUMBER( nscanned );
        OPDEBUG_APPEND_NUMBER( nscannedObjects );
        OPDEBUG_APPEND_BOOL( idhack );
        OPDEBUG_APPEND_BOOL( scanAndOrder );
        OPDEBUG_APPEND_BOOL( moved );
        OPDEBUG_APPEND_NUMBER( nmoved );
        OPDEBUG_APPEND_NUMBER( nMatched );
        OPDEBUG_APPEND_NUMBER( nModified );
        OPDEBUG_APPEND_NUMBER( ninserted );
        OPDEBUG_APPEND_NUMBER( ndeleted );
        OPDEBUG_APPEND_BOOL( fastmod );
        OPDEBUG_APPEND_BOOL( fastmodinsert );
        OPDEBUG_APPEND_BOOL( upsert );
        OPDEBUG_APPEND_BOOL( cursorExhausted );
        OPDEBUG_APPEND_NUMBER( keyUpdates );
        OPDEBUG_APPEND_NUMBER( writeConflicts );
        b.appendNumber("numYield", curop.numYields());

        {
            BSONObjBuilder locks(b.subobjStart("locks"));
            lockStats.report(&locks);
        }

        if (!exceptionInfo.empty()) {
            exceptionInfo.append(b, "exception", "exceptionCode");
        }

        OPDEBUG_APPEND_NUMBER( nreturned );
        OPDEBUG_APPEND_NUMBER( responseLength );
        b.append( "millis" , executionTime );

        execStats.append(b, "execStats");
    }

    void saveGLEStats(const BSONObj& result, const std::string& conn) {
        // This can be called in mongod, which is unfortunate.  To fix this,
        // we can redesign how connection pooling works on mongod for sharded operations.
    }
} // namespace mongo
