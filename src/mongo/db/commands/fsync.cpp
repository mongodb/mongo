// fsync.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/commands/fsync.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/commands.h"
#include "mongo/db/dur.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/background.h"

namespace mongo {
    
    class FSyncLockThread : public BackgroundJob {
        void doRealWork();
    public:
        FSyncLockThread() : BackgroundJob( true ) {}
        virtual ~FSyncLockThread(){}
        virtual string name() const { return "FSyncLockThread"; }
        virtual void run() {
            Client::initThread( "fsyncLockWorker" );
            try {
                doRealWork();
            }
            catch ( std::exception& e ) {
                error() << "FSyncLockThread exception: " << e.what() << endl;
            }
            cc().shutdown();
        }
    };

    /* see unlockFsync() for unlocking:
       db.$cmd.sys.unlock.findOne()
    */
    class FSyncCommand : public Command {
    public:
        static const char* url() { return "http://dochub.mongodb.org/core/fsynccommand"; }
        bool locked;
        bool pendingUnlock;
        SimpleMutex m; // protects locked var above
        string err;

        boost::condition _threadSync;
        boost::condition _unlockSync;

        FSyncCommand() : Command( "fsync" ), m("lockfsync") { locked=false; pendingUnlock=false; }
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual void help(stringstream& h) const { h << url(); }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::fsync);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {

            if (Lock::isLocked()) {
                errmsg = "fsync: Cannot execute fsync command from contexts that hold a data lock";
                return false;
            }

            bool sync = !cmdObj["async"].trueValue(); // async means do an fsync, but return immediately
            bool lock = cmdObj["lock"].trueValue();
            log() << "CMD fsync: sync:" << sync << " lock:" << lock << endl;
            if( lock ) {
                if ( ! sync ) {
                    errmsg = "fsync: sync option must be true when using lock";
                    return false;
                }

                SimpleMutex::scoped_lock lk(m);
                err = "";
                
                (new FSyncLockThread())->go();
                while ( ! locked && err.size() == 0 ) {
                    _threadSync.wait( m );
                }
                
                if ( err.size() ){
                    errmsg = err;
                    return false;
                }
                
                log() << "db is now locked for snapshotting, no writes allowed. db.fsyncUnlock() to unlock" << endl;
                log() << "    For more info see " << FSyncCommand::url() << endl;
                result.append("info", "now locked against writes, use db.fsyncUnlock() to unlock");
                result.append("seeAlso", FSyncCommand::url());

            }
            else {
                // the simple fsync command case
                if (sync) {
                    Lock::GlobalWrite w; // can this be GlobalRead? and if it can, it should be nongreedy.
                    getDur().commitNow();
                }
                // question : is it ok this is not in the dblock? i think so but this is a change from past behavior, 
                // please advise.
                result.append( "numFiles" , MemoryMappedFile::flushAll( sync ) );
            }
            return 1;
        }
    } fsyncCmd;

    SimpleMutex filesLockedFsync("filesLockedFsync");

    void FSyncLockThread::doRealWork() {
        SimpleMutex::scoped_lock lkf(filesLockedFsync);
        Lock::GlobalWrite global(true/*stopGreed*/);
        SimpleMutex::scoped_lock lk(fsyncCmd.m);
        
        verify( ! fsyncCmd.locked ); // impossible to get here if locked is true
        try { 
            getDur().syncDataAndTruncateJournal();
        } 
        catch( std::exception& e ) { 
            error() << "error doing syncDataAndTruncateJournal: " << e.what() << endl;
            fsyncCmd.err = e.what();
            fsyncCmd._threadSync.notify_one();
            fsyncCmd.locked = false;
            return;
        }
        
        global.downgrade();
        
        try {
            MemoryMappedFile::flushAll(true);
        }
        catch( std::exception& e ) { 
            error() << "error doing flushAll: " << e.what() << endl;
            fsyncCmd.err = e.what();
            fsyncCmd._threadSync.notify_one();
            fsyncCmd.locked = false;
            return;
        }

        verify( ! fsyncCmd.locked );
        fsyncCmd.locked = true;
        
        fsyncCmd._threadSync.notify_one();

        while ( ! fsyncCmd.pendingUnlock ) {
            fsyncCmd._unlockSync.wait(fsyncCmd.m);
        }
        fsyncCmd.pendingUnlock = false;
        
        fsyncCmd.locked = false;
        fsyncCmd.err = "unlocked";

        fsyncCmd._unlockSync.notify_one();
    }

    bool lockedForWriting() { 
        return fsyncCmd.locked; 
    }
    
    // @return true if unlocked
    bool _unlockFsync() {
        verify(!Lock::isLocked());
        SimpleMutex::scoped_lock lk( fsyncCmd.m );
        if( !fsyncCmd.locked ) { 
            return false;
        }
        fsyncCmd.pendingUnlock = true;
        fsyncCmd._unlockSync.notify_one();
        fsyncCmd._threadSync.notify_one();
        
        while ( fsyncCmd.locked ) {
            fsyncCmd._unlockSync.wait( fsyncCmd.m );
        }
        return true;
    }
}
