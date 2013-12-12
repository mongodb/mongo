// test_commands.cpp

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

#include "mongo/base/init.h"
#include "mongo/base/initializer_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/structure/collection.h"

namespace mongo {

    /* For testing only, not for general use. Enabled via command-line */
    class GodInsert : public Command {
    public:
        GodInsert() : Command( "godinsert" ) { }
        virtual bool adminOnly() const { return false; }
        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        // No auth needed because it only works when enabled via command line.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}
        virtual void help( stringstream &help ) const {
            help << "internal. for testing only.";
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string coll = cmdObj[ "godinsert" ].valuestrsafe();
            log() << "test only command godinsert invoked coll:" << coll << endl;
            uassert( 13049, "godinsert must specify a collection", !coll.empty() );
            string ns = dbname + "." + coll;
            BSONObj obj = cmdObj[ "obj" ].embeddedObjectUserCheck();

            Lock::DBWrite lk(ns);
            Client::Context ctx( ns );
            Database* db = ctx.db();
            Collection* collection = db->getCollection( ns );
            if ( !collection ) {
                collection = db->createCollection( ns );
                if ( !collection ) {
                    errmsg = "could not create collection";
                    return false;
                }
            }
            StatusWith<DiskLoc> res = collection->insertDocument( obj, false );
            return appendCommandStatus( result, res.getStatus() );
        }
    };

    /* for diagnostic / testing purposes. Enabled via command line. */
    class CmdSleep : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return true; }
        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream& help ) const {
            help << "internal testing command.  Makes db block (in a read lock) for 100 seconds\n";
            help << "w:true write lock. secs:<seconds>";
        }
        // No auth needed because it only works when enabled via command line.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}
        CmdSleep() : Command("sleep") { }
        bool run(const string& ns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            log() << "test only command sleep invoked" << endl;
            long long millis = 10 * 1000;

            if (cmdObj["secs"].isNumber() && cmdObj["millis"].isNumber()) {
                millis = cmdObj["secs"].numberLong() * 1000 + cmdObj["millis"].numberLong();
            }
            else if (cmdObj["secs"].isNumber()) {
                millis = cmdObj["secs"].numberLong() * 1000;
            }
            else if (cmdObj["millis"].isNumber()) {
                millis = cmdObj["millis"].numberLong();
            }

            if(cmdObj.getBoolField("w")) {
                Lock::GlobalWrite lk;
                sleepmillis(millis);
            }
            else {
                Lock::GlobalRead lk;
                sleepmillis(millis);
            }

            // Interrupt point for testing (e.g. maxTimeMS).
            killCurrentOp.checkForInterrupt();

            return true;
        }
    };

    // Testing only, enabled via command-line.
    class CapTrunc : public Command {
    public:
        CapTrunc() : Command( "captrunc" ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        // No auth needed because it only works when enabled via command line.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string coll = cmdObj[ "captrunc" ].valuestrsafe();
            uassert( 13416, "captrunc must specify a collection", !coll.empty() );
            string ns = dbname + "." + coll;
            int n = cmdObj.getIntField( "n" );

            // inclusive range?
            bool inc = cmdObj.getBoolField( "inc" );
            NamespaceDetails *nsd = nsdetails( ns );
            massert( 13417, "captrunc collection not found or empty", nsd);

            boost::scoped_ptr<Runner> runner(InternalPlanner::collectionScan(ns, InternalPlanner::BACKWARD));
            DiskLoc end;
            // We remove 'n' elements so the start is one past that
            for( int i = 0; i < n + 1; ++i ) {
                Runner::RunnerState state = runner->getNext(NULL, &end);
                massert( 13418, "captrunc invalid n", Runner::RUNNER_ADVANCED == state);
            }
            nsd->cappedTruncateAfter( ns.c_str(), end, inc );
            return true;
        }
    };

    // Testing-only, enabled via command line.
    class EmptyCapped : public Command {
    public:
        EmptyCapped() : Command( "emptycapped" ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool logTheOp() { return true; }
        // No auth needed because it only works when enabled via command line.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname, 
                                                     const BSONObj& cmdObj) {
            std::string systemIndexes = dbname + ".system.indexes";
            std::string coll = cmdObj[ "emptycapped" ].valuestrsafe();
            std::string ns = dbname + '.' + coll;
            BSONObj criteria = BSON("ns" << systemIndexes << "op" << "insert" << "insert.ns" << ns);

            return IndexBuilder::killMatchingIndexBuilds(criteria);
        }

        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string coll = cmdObj[ "emptycapped" ].valuestrsafe();
            uassert( 13428, "emptycapped must specify a collection", !coll.empty() );
            string ns = dbname + "." + coll;
            NamespaceDetails *nsd = nsdetails( ns );
            massert( 13429, "emptycapped no such collection", nsd );

            std::vector<BSONObj> indexes = stopIndexBuilds(dbname, cmdObj);

            nsd->emptyCappedCollection( ns.c_str() );

            IndexBuilder::restoreIndexes(dbname + ".system.indexes", indexes);

            return true;
        }
    };

    // ----------------------------

    MONGO_INITIALIZER(RegisterEmptyCappedCmd)(InitializerContext* context) {
        if (Command::testCommandsEnabled) {
            // Leaked intentionally: a Command registers itself when constructed.
            new CapTrunc();
            new CmdSleep();
            new EmptyCapped();
            new GodInsert();
        }
        return Status::OK();
    }


}
