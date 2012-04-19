// fsync.cpp

#include "mongo/db/d_concurrency.h"
#include "mongo/db/commands.h"
#include "mongo/db/dur.h"

namespace mongo {

    /* see unlockFsync() for unlocking:
       db.$cmd.sys.unlock.findOne()
    */
    class FSyncCommand : public Command {
        static const char* url() { return "http://www.mongodb.org/display/DOCS/fsync+Command"; }
    public:
        bool locked;
        SimpleMutex m; // protects locked var above
        FSyncCommand() : Command( "fsync" ), m("lockfsync") { locked=false; }
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual void help(stringstream& h) const { h << url(); }
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            bool sync = !cmdObj["async"].trueValue(); // async means do an fsync, but return immediately
            bool lock = cmdObj["lock"].trueValue();
            log() << "CMD fsync: sync:" << sync << " lock:" << lock << endl;
            if( lock ) {
                Lock::ThreadSpanningOp::setWLockedNongreedy();
                verify( !locked ); // impossible to get here if locked is true
                try { 
                    //uassert(12034, "fsync: can't lock while an unlock is pending", !unlockRequested);
                    uassert(12032, "fsync: sync option must be true when using lock", sync);
                    getDur().syncDataAndTruncateJournal();
                } catch(...) { 
                    Lock::ThreadSpanningOp::unsetW();
                    throw;
                }
                SimpleMutex::scoped_lock lk(m);
                Lock::ThreadSpanningOp::W_to_R();
                try {
                    MemoryMappedFile::flushAll(true);
                }
                catch(...) { 
                    Lock::ThreadSpanningOp::unsetR();
                    throw;
                }
                verify( !locked );
                locked = true;
                log() << "db is now locked for snapshotting, no writes allowed. db.fsyncUnlock() to unlock" << endl;
                log() << "    For more info see " << FSyncCommand::url() << endl;
                result.append("info", "now locked against writes, use db.fsyncUnlock() to unlock");
                result.append("seeAlso", url());
                Lock::ThreadSpanningOp::handoffR();
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

    bool lockedForWriting() { return fsyncCmd.locked; }

    // @return true if unlocked
    bool _unlockFsync() {
        SimpleMutex::scoped_lock lk(fsyncCmd.m);
        if( !fsyncCmd.locked ) { 
            return false;
        }
        fsyncCmd.locked = false;
        Lock::ThreadSpanningOp::unsetR();
        return true;
    }
}
