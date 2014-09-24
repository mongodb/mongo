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

#pragma once

#include "mongo/db/repl/consensus.h"
#include "mongo/db/repl/heartbeat_info.h"
#include "mongo/db/repl/manager.h"
#include "mongo/db/repl/member.h"
#include "mongo/db/repl/rs_base.h"
#include "mongo/db/repl/rs_config.h"
#include "mongo/db/repl/state_box.h"
#include "mongo/db/repl/sync_source_feedback.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/value.h"

namespace mongo {

    class Cloner;
    class OperationContext;

namespace repl {

    struct FixUpInfo;
    class OplogReader;
    class ReplSetSeedList;
    class ReplSetHealthPollTask;

    // information about the entire replset, such as the various servers in the set, and their state
    /* note: We currently do not free mem when the set goes away - it is assumed the replset is a
             singleton and long lived.
    */
    class ReplSetImpl : protected RSBase {
    public:
        // info on our state if the replset isn't yet "up".  for example, if we are pre-initiation.
        enum StartupStatus {
            PRESTART=0, LOADINGCONFIG=1, BADCONFIG=2, EMPTYCONFIG=3,
            EMPTYUNREACHABLE=4, STARTED=5, SOON=6
        };
        static StartupStatus startupStatus;
        static DiagStr startupStatusMsg;
        static string stateAsHtml(MemberState state);

        /* todo thread */
        void msgUpdateHBInfo(HeartbeatInfo);

        /**
         * Updates the lastHeartbeatRecv of Member with the given id.
         */
        void msgUpdateHBRecv(unsigned id, time_t newTime);

        void electCmdReceived(const StringData& set,
                              unsigned whoid,
                              int cfgver,
                              const OID& round,
                              BSONObjBuilder* result) {
            elect.electCmdReceived(set, whoid, cfgver, round, result);
        }

        StateBox box;

        SyncSourceFeedback syncSourceFeedback;

        OpTime lastOpTimeWritten;
        OpTime getEarliestOpTimeWritten() const;

        Status forceSyncFrom(const string& host, BSONObjBuilder* result);
        // Check if the current sync target is suboptimal. This must be called while holding a mutex
        // that prevents the sync source from changing.
        bool shouldChangeSyncTarget(const HostAndPort& target) const;

        /**
         * Find the closest member (using ping time) with a higher latest optime.
         */
        const Member* getMemberToSyncTo();
        void veto(const string& host, Date_t until);
        bool gotForceSync();
        void goStale(OperationContext* txn, const Member* m, const BSONObj& o);

        OID getElectionId() const { return elect.getElectionId(); }
        OpTime getElectionTime() const { return elect.getElectionTime(); }

        void loadLastOpTimeWritten(OperationContext* txn, bool quiet = false);
    private:
        set<ReplSetHealthPollTask*> healthTasks;
        void endOldHealthTasks();
        void startHealthTaskFor(Member *m);

        Consensus elect;
        void relinquish(OperationContext* txn);
        void forgetPrimary(OperationContext* txn);
    protected:
        bool _stepDown(OperationContext* txn, int secs);
        bool _freeze(int secs);
    private:
        void _assumePrimary();
        void changeState(MemberState s);

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
        void sethbmsg(const std::string& s, int logLevel = 0);

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
        bool iAmElectable() {
            lock lk(this);
            return _electableSet.find(_self->id()) != _electableSet.end();
        }
        bool isElectable(const unsigned id) {
            lock lk(this);
            return _electableSet.find(id) != _electableSet.end();
        }
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
        bool initFromConfig(OperationContext* txn, ReplSetConfig& c, bool reconf = false);
        void _fillIsMaster(BSONObjBuilder&);
        void _fillIsMasterHost(const Member*, vector<string>&, vector<string>&, vector<string>&);
        const ReplSetConfig& config() { return *_cfg; }
        string name() const { return _name; } /* @return replica set's logical name */
        int version() const { return _cfg->version; } /* @return replica set's config version */
        MemberState state() const { return box.getState(); }
        void _getOplogDiagsAsHtml(unsigned server_id, stringstream& ss) const;
        void _summarizeAsHtml(OperationContext* txn, stringstream&) const;
        void _summarizeStatus(BSONObjBuilder&) const; // for replSetGetStatus command

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
        bool _loadConfigFinish(OperationContext* txn, vector<ReplSetConfig*>& v);
        /**
         * Gather all possible configs (from command line seeds, our own config
         * doc, and any hosts listed therein) and try to initiate from the most
         * recent config we find.
         */
        void loadConfig(OperationContext* txn);

        list<HostAndPort> memberHostnames() const;
        bool iAmArbiterOnly() const { return myConfig().arbiterOnly; }
        bool iAmPotentiallyHot() const {
          return myConfig().potentiallyHot() && // not an arbiter
            elect.steppedDown <= time(0) && // not stepped down/frozen
            state() == MemberState::RS_SECONDARY; // not stale
        }
    protected:
        Member *_self;
        bool _buildIndexes;       // = _self->config().buildIndexes

        ReplSetImpl();
        /* throws exception if a problem initializing. */
        void init(OperationContext* txn, ReplSetSeedList&);

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
        /**
         * This forces a secondary to go into recovering state and stay there
         * until this is called again, passing in "false".  Multiple threads can
         * call this and it will leave maintenance mode once all of the callers
         * have called it again, passing in false.
         */
        bool setMaintenanceMode(OperationContext* txn, const bool inc);
        bool getMaintenanceMode() {
            lock rsLock( this );
            return _maintenanceMode > 0;
        }

    private:
        Member* head() const { return _members.head(); }
    public:
        const Member* findById(unsigned id) const;
        Member* getMutableMember(unsigned id);
        Member* findByName(const std::string& hostname) const;

        // Clears the vetoes (blacklisted sync sources)
        void clearVetoes();

    private:
        void _getTargets(list<Target>&, int &configVersion);
        void getTargets(list<Target>&, int &configVersion);
        void startThreads();
        friend class LegacyReplicationCoordinator;
        friend class Member;
        friend class Manager;
        friend class Consensus;

    private:
        bool _initialSyncClone(OperationContext* txn,
                               Cloner &cloner,
                               const std::string& host,
                               const list<string>& dbs,
                               bool dataPass);
        bool _initialSyncApplyOplog(OperationContext* txn,
                                    repl::SyncTail& syncer,
                                    OplogReader* r,
                                    const Member* source);
        void _initialSync();
        void _syncThread();
        void syncTail();
        void syncFixUp(OperationContext* txn, FixUpInfo& h, OplogReader& r);

    public:
        // keep a list of hosts that we've tried recently that didn't work
        map<string,time_t> _veto;

        // Allow index prefetching to be turned on/off
        enum IndexPrefetchConfig {
            PREFETCH_NONE=0, PREFETCH_ID_ONLY=1, PREFETCH_ALL=2
        };

        void setIndexPrefetchConfig(const IndexPrefetchConfig cfg) {
            _indexPrefetchConfig = cfg;
        }
        IndexPrefetchConfig getIndexPrefetchConfig() {
            return _indexPrefetchConfig;
        }

        const ReplSetConfig::MemberCfg& myConfig() const { return _config; }
        void syncThread();
        const OpTime lastOtherOpTime() const;
        /**
         * The most up to date electable replica
         */
        const OpTime lastOtherElectableOpTime() const;

        BSONObj getLastErrorDefault;
    private:
        IndexPrefetchConfig _indexPrefetchConfig;
    };
} // namespace repl
} // namespace mongo
