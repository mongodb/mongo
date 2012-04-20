// fsync.cpp

#include "pch.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/commands.h"
#include "mongo/db/dur.h"
#include "mongo/db/client.h"
#include "mongo/util/background.h"

namespace mongo {
    
    class FSyncLockThread : public BackgroundJob {
    public:
        FSyncLockThread() : BackgroundJob( true ) {}
        virtual ~FSyncLockThread(){}
        virtual string name() const { return "FSyncLockThread"; }
        void doRealWork();
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
        static const char* url() { return "http://www.mongodb.org/display/DOCS/fsync+Command"; }
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
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
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

    void FSyncLockThread::doRealWork() {
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
