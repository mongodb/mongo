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
#include "../../util/hostandport.h"
#include "../commands.h"
#include "rs_exception.h"
#include "rs_optime.h"
#include "rs_member.h"
#include "rs_config.h"

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
    public:
        Member(HostAndPort h, unsigned ord, const ReplSetConfig::MemberCfg *c, bool self);
        string fullName() const { return h().toString(); }
        const ReplSetConfig::MemberCfg& config() const { return *_config; }
        const HeartbeatInfo& hbinfo() const { return _hbinfo; }
        string lhb() { return _hbinfo.lastHeartbeatMsg; }
        MemberState state() const { return _hbinfo.hbstate; }
        const HostAndPort& h() const { return _h; }
        unsigned id() const { return _hbinfo.id(); }
        bool potentiallyHot() const { return _config->potentiallyHot(); } // not arbiter, not priority 0

        void summarizeAsHtml(stringstream& s) const;
        friend class ReplSetImpl;
    private:
        const ReplSetConfig::MemberCfg *_config; /* todo: when this changes??? */
        HostAndPort _h;
        HeartbeatInfo _hbinfo;
    };

    class Manager : public task::Server {
        ReplSetImpl *rs;
        bool busyWithElectSelf;
        int _primary;
        const Member* findOtherPrimary();
        void noteARemoteIsPrimary(const Member *);
        virtual void starting();
    public:
        Manager(ReplSetImpl *rs);
        void msgReceivedNewConfig(BSONObj);
        void msgCheckNewState();
    };

    struct Target;

    class Consensus {
        ReplSetImpl &rs;
        struct LastYea { 
            LastYea() : when(0), who(0xffffffff) { }
            time_t when;
            unsigned who;
        };
        Atomic<LastYea> ly;
        unsigned yea(unsigned memberId); // throws VoteException
        void _electSelf();
        bool weAreFreshest(bool& allUp, int& nTies);
        bool sleptLast; // slept last elect() pass
    public:
        Consensus(ReplSetImpl *t) : rs(*t) { 
            sleptLast = false;
        }
        int totalVotes() const;
        bool aMajoritySeemsToBeUp() const;
        void electSelf();
        void electCmdReceived(BSONObj, BSONObjBuilder*);
        void multiCommand(BSONObj cmd, list<Target>& L);
    };

    /** most operations on a ReplSet object should be done while locked. that logic implemented here. */
    class RSBase : boost::noncopyable { 
    private:
        mutex m;
        int _locked;
        ThreadLocalValue<bool> _lockedByMe;
    protected:
        RSBase() : m("RSBase"), _locked(0) { }

        class lock { 
            RSBase& rsbase;
            auto_ptr<scoped_lock> sl;
        public:
            lock(RSBase* b) : rsbase(*b) { 
                if( rsbase._lockedByMe.get() )
                    return; // recursive is ok...

                sl.reset( new scoped_lock(rsbase.m) );
                DEV assert(rsbase._locked == 0);
                rsbase._locked++; 
                rsbase._lockedByMe.set(true);
            }
            ~lock() { 
                if( sl.get() ) {
                    assert( rsbase._lockedByMe.get() );
                    DEV assert(rsbase._locked == 1);
                    rsbase._lockedByMe.set(false);
                    rsbase._locked--; 
                }
            }
        };

    public:
        /* for asserts */
        bool locked() const { return _locked != 0; }

        /* if true, is locked, and was locked by this thread. note if false, it could be in the lock or not for another 
           just for asserts & such so we can make the contracts clear on who locks what when.
           we don't use these locks that frequently, so the little bit of overhead is fine.
        */
        bool lockedByMe() { return _lockedByMe.get(); } 
    };

    class ReplSetHealthPollTask;

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
        static string startupStatusMsg;
        static string stateAsStr(MemberState state);
        static string stateAsHtml(MemberState state);

        /* todo thread */
        void msgUpdateHBInfo(HeartbeatInfo);
        bool isPrimary() const { return _myState == RS_PRIMARY; }
        bool isSecondary() const { return _myState == RS_SECONDARY; }

        //bool initiated() const { return curOpTime.initiated(); }

        OpTime lastOpTimeWritten;
        long long lastH; // hash we use to make sure we are reading the right flow of ops and aren't on an out-of-date "fork"
    private:
        set<ReplSetHealthPollTask*> healthTasks;
        void endOldHealthTasks();
        void startHealthTaskFor(Member *m);

    private:
        Consensus elect;
        bool ok() const { return _myState != RS_FATAL; }

        void relinquish();
        void assumePrimary();
        void loadLastOpTimeWritten();
        void changeState(MemberState s);

    protected:
        // "heartbeat message"
        // sent in requestHeartbeat respond in field "hbm" 
        char _hbmsg[256]; // we change this unocked, thus not a c++ string
    public:
        void sethbmsg(string s, int logLevel = 0) { 
            unsigned sz = s.size();
            if( sz >= 256 ) 
                memcpy(_hbmsg, s.c_str(), 255);
            else {
                _hbmsg[sz] = 0;
                memcpy(_hbmsg, s.c_str(), sz);
            }
            log(logLevel) << "replSet " << s << rsLog;
        }
    protected:
        bool initFromConfig(ReplSetConfig& c); // true if ok; throws if config really bad; false if config doesn't include self
        void _fillIsMaster(BSONObjBuilder&);
        void _fillIsMasterHost(const Member*, vector<string>&, vector<string>&, vector<string>&);
        const ReplSetConfig& config() { return *_cfg; }
        string name() const { return _name; } /* @return replica set's logical name */
        MemberState state() const { return _myState; }        
        void _fatal();
        void _getOplogDiagsAsHtml(unsigned server_id, stringstream& ss) const;
        void _summarizeAsHtml(stringstream&) const;        
        void _summarizeStatus(BSONObjBuilder&) const; // for replSetGetStatus command

        /* cfgString format is 
           replsetname/host1,host2:port,...
           where :port is optional.
           throws exception if a problem initializing. */
        ReplSetImpl(string cfgString);

        /* call afer constructing to start - returns fairly quickly after launching its threads */
        void _go();

    private:

        MemberState _myState;
        string _name;
        const vector<HostAndPort> *_seeds;
        ReplSetConfig *_cfg;

        /** load our configuration from admin.replset.  try seed machines too. 
            @return true if ok; throws if config really bad; false if config doesn't include self
        */
        bool _loadConfigFinish(vector<ReplSetConfig>& v);
        void loadConfig();

        list<HostAndPort> memberHostnames() const;
        const Member* currentPrimary() const { return _currentPrimary; }
        const ReplSetConfig::MemberCfg& myConfig() const { return _self->config(); }
        bool iAmArbiterOnly() const { return myConfig().arbiterOnly; }
        bool iAmPotentiallyHot() const { return myConfig().potentiallyHot(); }
        const Member *_currentPrimary;
        Member *_self;        
        List1<Member> _members; /* all members of the set EXCEPT self. */

    public:
        unsigned selfId() const { return _self->id(); }
        shared_ptr<Manager> mgr;

    private:
        Member* head() const { return _members.head(); }
        Member* findById(unsigned id) const;
        void _getTargets(list<Target>&, int &configVersion);
        void getTargets(list<Target>&, int &configVersion);
        void startThreads();
        friend class FeedbackThread;
        friend class CmdReplSetElect;
        friend class Member;
        friend class Manager;
        friend class Consensus;

    private:
        /* pulling data from primary related - see rs_sync.cpp */
        void _syncDoInitialSync();
        void syncDoInitialSync();
        void _syncThread();
        void syncTail();
        void syncApply(const BSONObj &o);
        void syncRollback(OplogReader& r);
        void syncFixUp(HowToFixUp& h, DBClientConnection*);
    public:
        void syncThread();
    };

    class ReplSet : public ReplSetImpl { 
    public:
        ReplSet(string cfgString) : ReplSetImpl(cfgString) { 
        }

        /* call after constructing to start - returns fairly quickly after la[unching its threads */
        void go() { _go(); }
        void fatal() { _fatal(); }
        bool isMaster(const char *client);
        MemberState state() const { return ReplSetImpl::state(); }
        string name() const { return ReplSetImpl::name(); }
        const ReplSetConfig& config() { return ReplSetImpl::config(); }
        void getOplogDiagsAsHtml(unsigned server_id, stringstream& ss) const { _getOplogDiagsAsHtml(server_id,ss); }
        void summarizeAsHtml(stringstream& ss) const { _summarizeAsHtml(ss); }
        void summarizeStatus(BSONObjBuilder& b) const  { _summarizeStatus(b); }
        void fillIsMaster(BSONObjBuilder& b) { _fillIsMaster(b); }

        /* we have a new config (reconfig) - apply it. 
           @param comment write a no-op comment to the oplog about it.  only makes sense if one is primary and initiating the reconf.
        */
        void haveNewConfig(ReplSetConfig& c, bool comment);

        /* if we delete old configs, this needs to assure locking. currently we don't so it is ok. */
        const ReplSetConfig& getConfig() { return config(); }

        bool lockedByMe() { return RSBase::lockedByMe(); }

        // heartbeat msg to send to others; descriptive diagnostic info
        string hbmsg() const { return _hbmsg; }
    };

    /** base class for repl set commands.  checks basic things such as in rs mode before the command 
        does its real work
        */
    class ReplSetCommand : public Command { 
    protected:
        ReplSetCommand(const char * s, bool show=false) : Command(s) { }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual bool logTheOp() { return false; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream &help ) const { help << "internal"; }
        bool check(string& errmsg, BSONObjBuilder& result) {
            if( !replSet ) { 
                errmsg = "not running with --replSet";
                return false;
            }
            if( theReplSet == 0 ) {
                result.append("startupStatus", ReplSet::startupStatus);
                errmsg = ReplSet::startupStatusMsg.empty() ? "replset unknown error 2" : ReplSet::startupStatusMsg;
                return false;
            }
            return true;
        }
    };

    /** helpers ----------------- */
    
    void parseReplsetCmdLine(string cfgString, string& setname, vector<HostAndPort>& seeds, set<HostAndPort>& seedSet );

    /** inlines ----------------- */

    inline Member::Member(HostAndPort h, unsigned ord, const ReplSetConfig::MemberCfg *c, bool self) : 
        _config(c), _h(h), _hbinfo(ord) { 
            if( self ) { 
                _hbinfo.health = 1.0;
            }
    }

    inline bool ReplSet::isMaster(const char *client) {         
        /* todo replset */
        return isPrimary();
    }

}
