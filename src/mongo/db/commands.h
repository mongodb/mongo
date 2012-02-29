// commands.h

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

#include "jsobj.h"
#include "../util/mongoutils/str.h"

namespace mongo {

    class BSONObj;
    class BSONObjBuilder;
    class Client;
    class Timer;

    /** mongodb "commands" (sent via db.$cmd.findOne(...))
        subclass to make a command.  define a singleton object for it.
        */
    class Command {
    protected:
        string parseNsFullyQualified(const string& dbname, const BSONObj& cmdObj) const;
    public:
        // only makes sense for commands where 1st parm is the collection.
        virtual string parseNs(const string& dbname, const BSONObj& cmdObj) const;

        // warning: isAuthorized uses the lockType() return values, and values are being passed 
        // around as ints so be careful as it isn't really typesafe and will need cleanup later
        enum LockType { READ = -1 , NONE = 0 , WRITE = 1 };

        const string name;

        /* run the given command
           implement this...

           fromRepl - command is being invoked as part of replication syncing.  In this situation you
                      normally do not want to log the command to the local oplog.

           return value is true if succeeded.  if false, set errmsg text.
        */
        virtual bool run(const string& db, BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool fromRepl = false ) = 0;

        /*
           note: logTheOp() MUST be false if READ
           if NONE, can't use Client::Context setup
                    use with caution
         */
        virtual LockType locktype() const = 0;

        /** if true, lock globally instead of just the one database. by default only the one 
            database will be locked. 
        */
        virtual bool lockGlobally() const { return false; }

        /* Return true if only the admin ns has privileges to run this command. */
        virtual bool adminOnly() const {
            return false;
        }

        void htmlHelp(stringstream&) const;

        /* Like adminOnly, but even stricter: we must either be authenticated for admin db,
           or, if running without auth, on the local interface.  Used for things which 
           are so major that remote invocation may not make sense (e.g., shutdownServer).

           When localHostOnlyIfNoAuth() is true, adminOnly() must also be true.
        */
        virtual bool localHostOnlyIfNoAuth(const BSONObj& cmdObj) { return false; }

        /* Return true if slaves are allowed to execute the command
           (the command directly from a client -- if fromRepl, always allowed).
        */
        virtual bool slaveOk() const = 0;

        /* Return true if the client force a command to be run on a slave by
           turning on the 'slaveOk' option in the command query.
        */
        virtual bool slaveOverrideOk() const {
            return false;
        }

        /* Override and return true to if true,log the operation (logOp()) to the replication log.
           (not done if fromRepl of course)

           Note if run() returns false, we do NOT log.
        */
        virtual bool logTheOp() { return false; }

        virtual void help( stringstream& help ) const;

        /* Return true if authentication and security applies to the commands.  Some commands
           (e.g., getnonce, authenticate) can be done by anyone even unauthorized.
        */
        virtual bool requiresAuth() { return true; }

        /* Return true if a replica set secondary should go into "recovering"
           (unreadable) state while running this command.
         */
        virtual bool maintenanceMode() const { return false; }

        /* Return true if command should be permitted when a replica set secondary is in "recovering"
           (unreadable) state.
         */
        virtual bool maintenanceOk() const { return true; /* assumed true prior to commit */ }

        /** @param webUI expose the command in the web ui as localhost:28017/<name>
            @param oldName an optional old, deprecated name for the command
        */
        Command(const char *_name, bool webUI = false, const char *oldName = 0);

        virtual ~Command() {}

    protected:
        BSONObj getQuery( const BSONObj& cmdObj ) {
            if ( cmdObj["query"].type() == Object )
                return cmdObj["query"].embeddedObject();
            if ( cmdObj["q"].type() == Object )
                return cmdObj["q"].embeddedObject();
            return BSONObj();
        }

        static void logIfSlow( const Timer& cmdTimer,  const string& msg);

        static map<string,Command*> * _commands;
        static map<string,Command*> * _commandsByBestName;
        static map<string,Command*> * _webCommands;

    public:
        static const map<string,Command*>* commandsByBestName() { return _commandsByBestName; }
        static const map<string,Command*>* webCommands() { return _webCommands; }
        /** @return if command was found and executed */
        static bool runAgainstRegistered(const char *ns, BSONObj& jsobj, BSONObjBuilder& anObjBuilder, int queryOptions = 0);
        static LockType locktype( const string& name );
        static Command * findCommand( const string& name );
    };

    class CmdShutdown : public Command {
    public:
        virtual bool requiresAuth() { return true; }
        virtual bool adminOnly() const { return true; }
        virtual bool localHostOnlyIfNoAuth(const BSONObj& cmdObj) { return true; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream& help ) const;
        CmdShutdown() : Command("shutdown") {}
        bool run(const string& dbname, BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool fromRepl);
    private:
        bool shutdownHelper();
    };

    bool _runCommands(const char *ns, BSONObj& jsobj, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions);

} // namespace mongo
