// /db/repl/rs.h

/**
*    Copyright (C) 2008 10gen Inc.
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
*/

#pragma once

#include "../../util/concurrency/list.h"
#include "../../util/concurrency/value.h"
#include "../../util/concurrency/msg.h"
#include "../../util/net/hostandport.h"
#include "../commands.h"
#include "../oplog.h"
#include "../oplogreader.h"
#include "rs_exception.h"
#include "rs_optime.h"
#include "rs_member.h"
#include "rs_config.h"

/**
 * Order of Events
 *
 * On startup, if the --replSet option is present, startReplSets is called.
 * startReplSets forks off a new thread for replica set activities.  It creates
 * the global theReplSet variable and calls go() on it.
 *
 * theReplSet's constructor changes the replica set's state to RS_STARTUP,
 * starts the replica set manager, and loads the config (if the replica set
 * has been initialized).
 */

namespace mongo {

    struct HowToFixUp;
    struct Target;
    class DBClientConnection;
    class ReplSetImpl;
    class OplogReader;
    extern bool replSet; // true if using repl sets
    extern class ReplSet *theReplSet; // null until initialized
    extern Tee *rsLog;

    /* member of a replica set */
    class Member : public List1<Member>::Base {
    private:
        ~Member(); // intentionally unimplemented as should never be called -- see List1<>::Base.
        Member(const Member&); 
    public:
        Member(HostAndPort h, unsigned ord, const ReplSetConfig::MemberCfg *c, bool self);

        string fullName() const { return h().toString(); }
        const ReplSetConfig::MemberCfg& config() const { return _config; }
        ReplSetConfig::MemberCfg& configw() { return _config; }
        const HeartbeatInfo& hbinfo() const { return _hbinfo; }
        HeartbeatInfo& get_hbinfo() { return _hbinfo; }
        string lhb() const { return _hbinfo.lastHeartbeatMsg; }
        MemberState state() const { return _hbinfo.hbstate; }
        const HostAndPort& h() const { return _h; }
        unsigned id() const { return _hbinfo.id(); }

        bool potentiallyHot() const { return _config.potentiallyHot(); } // not arbiter, not priority 0
        void summarizeMember(stringstream& s) const;

    private:
        friend class ReplSetImpl;
        ReplSetConfig::MemberCfg _config;
        const HostAndPort _h;
        HeartbeatInfo _hbinfo;
    };

    namespace replset {
        /**
         * "Normal" replica set syncing
         */
        class SyncTail : public Sync {
        public:
            virtual ~SyncTail() {}
            SyncTail(const string& host) : Sync(host) {}
            virtual bool syncApply(const BSONObj &o);
        };

        /**
         * Initial clone and sync
         */
        class InitialSync : public SyncTail {
        public:
            InitialSync(const string& host) : SyncTail(host) {}
            virtual ~InitialSync() {}
            bool oplogApplication(OplogReader& r, const Member* source, const OpTime& applyGTE, const OpTime& minValid);
            virtual void applyOp(const BSONObj& o, const OpTime& minvalid);
        };

        // TODO: move hbmsg into an error-keeping class (SERVER-4444)
        void sethbmsg(const string& s, const int logLevel=0);

    } // namespace replset

    class Manager : public task::Server {
        ReplSetImpl *rs;
        bool busyWithElectSelf;
        int _primary;

        /** @param two - if true two primaries were seen.  this can happen transiently, in addition to our
                         polling being only occasional.  in this case null is returned, but the caller should
                         not assume primary itself in that situation.
        */
        const Member* findOtherPrimary(bool& two);

        void noteARemoteIsPrimary(const Member *);
        void checkElectableSet();
        void checkAuth();
        virtual void starting();
    public:
        Manager(ReplSetImpl *rs);
        virtual ~Manager();
        void msgReceivedNewConfig(BSONObj);
        void msgCheckNewState();
    };

    class GhostSync : public task::Server {
        struct GhostSlave : boost::noncopyable {
            GhostSlave() : last(0), slave(0), init(false) { }
            OplogReader reader;
            OpTime last;
            Member* slave;
            bool init;
        };
        /**
         * This is a cache of ghost slaves
         */
        typedef map< mongo::OID,shared_ptr<GhostSlave> > MAP;
        MAP _ghostCache;
        RWLock _lock; // protects _ghostCache
        ReplSetImpl *rs;
        virtual void starting();
    public:
        GhostSync(ReplSetImpl *_rs) : task::Server("rsGhostSync"), _lock("GhostSync"), rs(_rs) {}
        ~GhostSync() {
            log() << "~GhostSync() called" << rsLog;
        }

        /**
         * Replica sets can sync in a hierarchical fashion, which throws off w
         * calculation on the master.  percolate() faux-syncs from an upstream
         * node so that the primary will know what the slaves are up to.
         *
         * We can't just directly sync to the primary because it could be
         * unreachable, e.g., S1--->S2--->S3--->P.  S2 should ghost sync from S3
         * and S3 can ghost sync from the primary.
         *
         * Say we have an S1--->S2--->P situation and this node is S2.  rid
         * would refer to S1.  S2 would create a ghost slave of S1 and connect
         * it to P (_currentSyncTarget). Then it would use this connection to
         * pretend to be S1, replicating off of P.
         */
        void percolate(const BSONObj& rid, const OpTime& last);
        void associateSlave(const BSONObj& rid, const int memberId);
        void updateSlave(const mongo::OID& id, const OpTime& last);
    };

    struct Target;

    class Consensus {
        ReplSetImpl &rs;
        struct LastYea {
            LastYea() : when(0), who(0xffffffff) { }
            time_t when;
            unsigned who;
        };
        static SimpleMutex lyMutex;
        Guarded<LastYea,lyMutex> ly;
        unsigned yea(unsigned memberId); // throws VoteException
        void electionFailed(unsigned meid);
        void _electSelf();
        bool weAreFreshest(bool& allUp, int& nTies);
        bool sleptLast; // slept last elect() pass
    public:
        Consensus(ReplSetImpl *t) : rs(*t) {
            sleptLast = false;
            steppedDown = 0;
        }

        /* if we've stepped down, this is when we are allowed to try to elect ourself again.
           todo: handle possible weirdnesses at clock skews etc.
        */
        time_t steppedDown;

        int totalVotes() const;
        bool aMajoritySeemsToBeUp() const;
        bool shouldRelinquish() const;
        void electSelf();
        void electCmdReceived(BSONObj, BSONObjBuilder*);
        void multiCommand(BSONObj cmd, list<Target>& L);
    };

    /**
     * most operations on a ReplSet object should be done while locked. that
     * logic implemented here.
     *
     * Order of locking: lock the replica set, then take a rwlock.
     */
    class RSBase : boost::noncopyable {
    public:
        const unsigned magic;
        void assertValid() { verify( magic == 0x12345677 ); }
    private:
        mongo::mutex m;
        int _locked;
        ThreadLocalValue<bool> _lockedByMe;
    protected:
        RSBase() : magic(0x12345677), m("RSBase"), _locked(0) { }
        ~RSBase() {
            /* this can happen if we throw in the constructor; otherwise never happens.  thus we log it as it is quite unusual. */
            log() << "replSet ~RSBase called" << rsLog;
        }

    public:
        class lock {
            RSBase& rsbase;
            auto_ptr<scoped_lock> sl;
        public:
            lock(RSBase* b) : rsbase(*b) {
                if( rsbase._lockedByMe.get() )
                    return; // recursive is ok...

                sl.reset( new scoped_lock(rsbase.m) );
                DEV verify(rsbase._locked == 0);
                rsbase._locked++;
                rsbase._lockedByMe.set(true);
            }
            ~lock() {
                if( sl.get() ) {
                    verify( rsbase._lockedByMe.get() );
                    DEV verify(rsbase._locked == 1);
                    rsbase._lockedByMe.set(false);
                    rsbase._locked--;
                }
            }
        };

        /* for asserts */
        bool locked() const { return _locked != 0; }

        /* if true, is locked, and was locked by this thread. note if false, it could be in the lock or not for another
           just for asserts & such so we can make the contracts clear on who locks what when.
           we don't use these locks that frequently, so the little bit of overhead is fine.
        */
        bool lockedByMe() { return _lockedByMe.get(); }
    };

    class ReplSetHealthPollTask;

    /* safe container for our state that keeps member pointer and state variables always aligned */
    class StateBox : boost::noncopyable {
    public:
        struct SP { // SP is like pair<MemberState,const Member *> but nicer
            SP() : state(MemberState::RS_STARTUP), primary(0) { }
            MemberState state;
            const Member *primary;
        };
        const SP get() {
            rwlock lk(m, false);
            return sp;
        }
        MemberState getState() const {
            rwlock lk(m, false);
            return sp.state;
        }
        const Member* getPrimary() const {
            rwlock lk(m, false);
            return sp.primary;
        }
        void change(MemberState s, const Member *self) {
            rwlock lk(m, true);
            if( sp.state != s ) {
                log() << "replSet " << s.toString() << rsLog;
            }
            sp.state = s;
            if( s.primary() ) {
                sp.primary = self;
            }
            else {
                if( self == sp.primary )
                    sp.primary = 0;
            }
        }
        void set(MemberState s, const Member *p) {
            rwlock lk(m, true);
            sp.state = s;
            sp.primary = p;
        }
        void setSelfPrimary(const Member *self) { change(MemberState::RS_PRIMARY, self); }
        void setOtherPrimary(const Member *mem) {
            rwlock lk(m, true);
            verify( !sp.state.primary() );
            sp.primary = mem;
        }
        void noteRemoteIsPrimary(const Member *remote) {
            rwlock lk(m, true);
            if( !sp.state.secondary() && !sp.state.fatal() )
                sp.state = MemberState::RS_RECOVERING;
            sp.primary = remote;
        }
        StateBox() : m("StateBox") { }
    private:
        RWLock m;
        SP sp;
    };

    void parseReplsetCmdLine(string cfgString, string& setname, vector<HostAndPort>& seeds, set<HostAndPort>& seedSet );

    /** Parameter given to the --replSet command line option (parsed).
        Syntax is "<setname>/<seedhost1>,<seedhost2>"
        where setname is a name and seedhost is "<host>[:<port>]" */
    class ReplSetCmdline {
    public:
        ReplSetCmdline(string cfgString) { parseReplsetCmdLine(cfgString, setname, seeds, seedSet); }
        string setname;
        vector<HostAndPort> seeds;
        set<HostAndPort> seedSet;
    };

    /* information about the entire repl set, such as the various servers in the set, and their state */
    /* note: We currently do not free mem when the set goes away - it is assumed the replset is a
             singleton and long lived.
    */
    class ReplSetImpl : protected RSBase {
    public:
        /** info on our state if the replset isn't yet "up".  for example, if we are pre-initiation. */
        enum StartupStatus {
            PRESTART=0, LOADINGCONFIG=1, BADCONFIG=2, EMPTYCONFIG=3,
            EMPTYUNREACHABLE=4, STARTED=5, SOON=6
        };
        static StartupStatus startupStatus;
        static DiagStr startupStatusMsg;
        static string stateAsHtml(MemberState state);

        /* todo thread */
        void msgUpdateHBInfo(HeartbeatInfo);

        StateBox box;

        OpTime lastOpTimeWritten;
        long long lastH; // hash we use to make sure we are reading the right flow of ops and aren't on an out-of-date "fork"
        bool forceSyncFrom(const string& host, string& errmsg, BSONObjBuilder& result);
    private:
        set<ReplSetHealthPollTask*> healthTasks;
        void endOldHealthTasks();
        void startHealthTaskFor(Member *m);

        Consensus elect;
        void relinquish();
        void forgetPrimary();
    protected:
        bool _stepDown(int secs);
        bool _freeze(int secs);
    private:
        void assumePrimary();
        void loadLastOpTimeWritten(bool quiet=false);
        void changeState(MemberState s);
        
        /**
         * Find the closest member (using ping time) with a higher latest optime.
         */
        Member* getMemberToSyncTo();
        void veto(const string& host, unsigned secs=10);
        Member* _currentSyncTarget;
        Member* _forceSyncTarget;

        bool _blockSync;
        void blockSync(bool block);

        // set of electable members' _ids
        set<unsigned> _electableSet;
    protected:
        // "heartbeat message"
        // sent in requestHeartbeat respond in field "hbm"
        char _hbmsg[256]; // we change this unlocked, thus not an stl::string
        time_t _hbmsgTime; // when it was logged
    public:
        void sethbmsg(string s, int logLevel = 0);

        /**
         * Election with Priorities
         *
         * Each node (n) keeps a set of nodes that could be elected primary.
         * Each node in this set:
         *
         *  1. can connect to a majority of the set
         *  2. has a priority greater than 0
         *  3. has an optime within 10 seconds of the most up-to-date node
         *     that n can reach
         *
         * If a node fails to meet one or more of these criteria, it is removed
         * from the list.  This list is updated whenever the node receives a
         * heartbeat.
         *
         * When a node sends an "am I freshest?" query, the node receiving the
         * query checks their electable list to make sure that no one else is
         * electable AND higher priority.  If this check passes, the node will
         * return an "ok" response, if not, it will veto.
         *
         * If a node is primary and there is another node with higher priority
         * on the electable list (i.e., it must be synced to within 10 seconds
         * of the current primary), the node (or nodes) with connections to both
         * the primary and the secondary with higher priority will issue
         * replSetStepDown requests to the primary to allow the higher-priority
         * node to take over.  
         */
        void addToElectable(const unsigned m) { lock lk(this); _electableSet.insert(m); }
        void rmFromElectable(const unsigned m) { lock lk(this); _electableSet.erase(m); }
        bool iAmElectable() { lock lk(this); return _electableSet.find(_self->id()) != _electableSet.end(); }
        bool isElectable(const unsigned id) { lock lk(this); return _electableSet.find(id) != _electableSet.end(); }
        Member* getMostElectable();
    protected:
        /**
         * Load a new config as the replica set's main config.
         *
         * If there is a "simple" change (just adding a node), this shortcuts
         * the config. Returns true if the config was changed.  Returns false
         * if the config doesn't include a this node.  Throws an exception if
         * something goes very wrong.
         *
         * Behavior to note:
         *  - locks this
         *  - intentionally leaks the old _cfg and any old _members (if the
         *    change isn't strictly additive)
         */
        bool initFromConfig(ReplSetConfig& c, bool reconf=false); 
        void _fillIsMaster(BSONObjBuilder&);
        void _fillIsMasterHost(const Member*, vector<string>&, vector<string>&, vector<string>&);
        const ReplSetConfig& config() { return *_cfg; }
        string name() const { return _name; } /* @return replica set's logical name */
        MemberState state() const { return box.getState(); }
        void _fatal();
        void _getOplogDiagsAsHtml(unsigned server_id, stringstream& ss) const;
        void _summarizeAsHtml(stringstream&) const;
        void _summarizeStatus(BSONObjBuilder&) const; // for replSetGetStatus command

        /* throws exception if a problem initializing. */
        ReplSetImpl(ReplSetCmdline&);

        /* call afer constructing to start - returns fairly quickly after launching its threads */
        void _go();

    private:
        string _name;
        const vector<HostAndPort> *_seeds;
        ReplSetConfig *_cfg;

        /**
         * Finds the configuration with the highest version number and attempts
         * load it.
         */
        bool _loadConfigFinish(vector<ReplSetConfig>& v);
        /**
         * Gather all possible configs (from command line seeds, our own config
         * doc, and any hosts listed therein) and try to initiate from the most
         * recent config we find.
         */
        void loadConfig();

        list<HostAndPort> memberHostnames() const;
        const ReplSetConfig::MemberCfg& myConfig() const { return _config; }
        bool iAmArbiterOnly() const { return myConfig().arbiterOnly; }
        bool iAmPotentiallyHot() const {
          return myConfig().potentiallyHot() && // not an arbiter
            elect.steppedDown <= time(0) && // not stepped down/frozen
            state() == MemberState::RS_SECONDARY; // not stale
        }
    protected:
        Member *_self;
        bool _buildIndexes;       // = _self->config().buildIndexes
        void setSelfTo(Member *); // use this as it sets buildIndexes var
    private:
        List1<Member> _members; // all members of the set EXCEPT _self.
        ReplSetConfig::MemberCfg _config; // config of _self
        unsigned _id; // _id of _self

        int _maintenanceMode; // if we should stay in recovering state
    public:
        // this is called from within a writelock in logOpRS
        unsigned selfId() const { return _id; }
        Manager *mgr;
        GhostSync *ghost;
        /**
         * This forces a secondary to go into recovering state and stay there
         * until this is called again, passing in "false".  Multiple threads can
         * call this and it will leave maintenance mode once all of the callers
         * have called it again, passing in false.
         */
        void setMaintenanceMode(const bool inc);
    private:
        Member* head() const { return _members.head(); }
    public:
        const Member* findById(unsigned id) const;
    private:
        void _getTargets(list<Target>&, int &configVersion);
        void getTargets(list<Target>&, int &configVersion);
        void startThreads();
        friend class FeedbackThread;
        friend class CmdReplSetElect;
        friend class Member;
        friend class Manager;
        friend class GhostSync;
        friend class Consensus;

    private:
        bool initialSyncOplogApplication(const OpTime& applyGTE, const OpTime& minValid);
        void _syncDoInitialSync();
        void syncDoInitialSync();
        void _syncThread();
        bool tryToGoLiveAsASecondary(OpTime&); // readlocks
        void syncTail();
        unsigned _syncRollback(OplogReader& r);
        void syncRollback(OplogReader& r);
        void syncFixUp(HowToFixUp& h, OplogReader& r);

        // get an oplog reader for a server with an oplog entry timestamp greater
        // than or equal to minTS, if set.
        Member* _getOplogReader(OplogReader& r, const OpTime& minTS);

        // check lastOpTimeWritten against the remote's earliest op, filling in
        // remoteOldestOp.
        bool _isStale(OplogReader& r, const OpTime& minTS, BSONObj& remoteOldestOp);

        // keep a list of hosts that we've tried recently that didn't work
        map<string,time_t> _veto;
    public:
        void syncThread();
        const OpTime lastOtherOpTime() const;
    };

    class ReplSet : public ReplSetImpl {
    public:
        ReplSet(ReplSetCmdline& replSetCmdline) : ReplSetImpl(replSetCmdline) {  }

        // for the replSetStepDown command
        bool stepDown(int secs) { return _stepDown(secs); }

        // for the replSetFreeze command
        bool freeze(int secs) { return _freeze(secs); }

        string selfFullName() {
            verify( _self );
            return _self->fullName();
        }

        bool buildIndexes() const { return _buildIndexes; }

        /* call after constructing to start - returns fairly quickly after la[unching its threads */
        void go() { _go(); }

        void fatal() { _fatal(); }
        bool isPrimary() { return box.getState().primary(); }
        bool isSecondary() {  return box.getState().secondary(); }
        MemberState state() const { return ReplSetImpl::state(); }
        string name() const { return ReplSetImpl::name(); }
        const ReplSetConfig& config() { return ReplSetImpl::config(); }
        void getOplogDiagsAsHtml(unsigned server_id, stringstream& ss) const { _getOplogDiagsAsHtml(server_id,ss); }
        void summarizeAsHtml(stringstream& ss) const { _summarizeAsHtml(ss); }
        void summarizeStatus(BSONObjBuilder& b) const  { _summarizeStatus(b); }
        void fillIsMaster(BSONObjBuilder& b) { _fillIsMaster(b); }

        /**
         * We have a new config (reconfig) - apply it.
         * @param comment write a no-op comment to the oplog about it.  only
         * makes sense if one is primary and initiating the reconf.
         *
         * The slaves are updated when they get a heartbeat indicating the new
         * config.  The comment is a no-op.
         */
        void haveNewConfig(ReplSetConfig& c, bool comment);

        /**
         * Pointer assignment isn't necessarily atomic, so this needs to assure
         * locking, even though we don't delete old configs.
         */
        const ReplSetConfig& getConfig() { return config(); }

        bool lockedByMe() { return RSBase::lockedByMe(); }

        // heartbeat msg to send to others; descriptive diagnostic info
        string hbmsg() const {
            if( time(0)-_hbmsgTime > 120 ) return "";
            return _hbmsg;
        }
    };

    /**
     * Base class for repl set commands.  Checks basic things such if we're in
     * rs mode before the command does its real work.
     */
    class ReplSetCommand : public Command {
    protected:
        ReplSetCommand(const char * s, bool show=false) : Command(s, show) { }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual bool logTheOp() { return false; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream &help ) const { help << "internal"; }

        /**
         * Some replica set commands call this and then call check(). This is
         * intentional, as they might do things before theReplSet is initialized
         * that still need to be checked for auth.
         */
        bool checkAuth(string& errmsg, BSONObjBuilder& result) {
            if( !noauth ) {
                AuthenticationInfo *ai = cc().getAuthenticationInfo();
                if (!ai->isAuthorizedForLock("admin", locktype())) {
                    errmsg = "replSet command unauthorized";
                    return false;
                }
            }
            return true;
        }

        bool check(string& errmsg, BSONObjBuilder& result) {
            if( !replSet ) {
                errmsg = "not running with --replSet";
                if( cmdLine.configsvr ) { 
                    result.append("info", "configsvr"); // for shell prompt
                }
                return false;
            }

            if( theReplSet == 0 ) {
                result.append("startupStatus", ReplSet::startupStatus);
                string s;
                errmsg = ReplSet::startupStatusMsg.empty() ? "replset unknown error 2" : ReplSet::startupStatusMsg.get();
                if( ReplSet::startupStatus == 3 )
                    result.append("info", "run rs.initiate(...) if not yet done for the set");
                return false;
            }

            return checkAuth(errmsg, result);
        }
    };

    /**
     * does local authentication
     * directly authorizes against AuthenticationInfo
     */
    void replLocalAuth();

    /** inlines ----------------- */

    inline Member::Member(HostAndPort h, unsigned ord, const ReplSetConfig::MemberCfg *c, bool self) :
        _config(*c), _h(h), _hbinfo(ord) {
        verify(c);
        if( self )
            _hbinfo.health = 1.0;
    }

}
