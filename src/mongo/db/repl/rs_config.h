// rs_config.h
// repl set configuration
//

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

#include "mongo/db/jsobj.h"
#include "mongo/util/concurrency/list.h"
#include "mongo/util/concurrency/race.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {
    class Member;
    const std::string rsConfigNs = "local.system.replset";

    class ReplSetConfig {
        enum { EMPTYCONFIG = -2 };
        struct TagSubgroup;

        // Protects _groups.
        static mongo::mutex groupMx;
    public:
        /**
         * This contacts the given host and tries to get a config from them.
         *
         * This sends a test heartbeat to the host and, if all goes well and the
         * host has a more recent config, fetches the config and loads it (see
         * from().
         *
         * If it's contacting itself, it skips the heartbeat (for obvious
         * reasons.) If something is misconfigured, throws an exception. If the
         * host couldn't be queried or is just blank, ok() will be false.
         */
        static ReplSetConfig* make(const HostAndPort& h);

        static ReplSetConfig* make(BSONObj cfg, bool force=false);

        /**
         * This uses DBDirectClient to check itself for a config.  This way we don't need to connect
         * to ourselves over the network to fetch our own config.
         */
        static ReplSetConfig* makeDirect();

        bool ok() const { return _ok; }

        struct TagRule;

        struct MemberCfg {
            MemberCfg() : _id(-1), votes(1), priority(1.0), arbiterOnly(false), slaveDelay(0), hidden(false), buildIndexes(true) { }
            int _id;              /* ordinal */
            unsigned votes;       /* how many votes this node gets. default 1. */
            HostAndPort h;
            double priority;      /* 0 means can never be primary */
            bool arbiterOnly;
            int slaveDelay;       /* seconds.  int rather than unsigned for convenient to/front bson conversion. */
            bool hidden;          /* if set, don't advertise to drives in isMaster. for non-primaries (priority 0) */
            bool buildIndexes;    /* if false, do not create any non-_id indexes */
            std::map<std::string,std::string> tags;     /* tagging for data center, rack, etc. */
        private:
            std::set<TagSubgroup*> _groups; // the subgroups this member belongs to
        public:
            const std::set<TagSubgroup*>& groups() const {
                return _groups;
            }
            std::set<TagSubgroup*>& groupsw() {
                return _groups;
            }
            void check() const;   /* check validity, assert if not. */
            BSONObj asBson() const;
            bool potentiallyHot() const { return !arbiterOnly && priority > 0; }
            void updateGroups(const OpTime& last) {
                RACECHECK
                scoped_lock lk(ReplSetConfig::groupMx);
                for (std::set<TagSubgroup*>::const_iterator it = groups().begin(); it != groups().end(); it++) {
                    (*it)->updateLast(last);
                }
            }
            bool isSameIgnoringTags(const MemberCfg& r) const {
                return _id == r._id && votes == r.votes && h == r.h && priority == r.priority &&
                       arbiterOnly == r.arbiterOnly && slaveDelay == r.slaveDelay &&
                       hidden == r.hidden && buildIndexes == r.buildIndexes;
            }
            bool operator==(const MemberCfg& r) const {
                if (!tags.empty() || !r.tags.empty()) {
                    if (tags.size() != r.tags.size()) {
                        return false;
                    }

                    // if they are the same size and not equal, at least one
                    // element in A must be different in B
                    for (std::map<std::string,std::string>::const_iterator lit = tags.begin(); lit != tags.end(); lit++) {
                        std::map<std::string,std::string>::const_iterator rit = r.tags.find((*lit).first);

                        if (rit == r.tags.end() || (*lit).second != (*rit).second) {
                            return false;
                        }
                    }
                }

                return isSameIgnoringTags(r);
            }
            bool operator!=(const MemberCfg& r) const { return !(*this == r); }
        };

        std::vector<MemberCfg> members;
        std::string _id;
        int version;

        BSONObj getLastErrorDefaults;
        std::map<std::string,TagRule*> rules;

        std::list<HostAndPort> otherMemberHostnames() const; // except self

        /** @return true if could connect, and there is no cfg object there at all */
        bool empty() const { return version == EMPTYCONFIG; }

        std::string toString() const { return asBson().toString(); }

        /** validate the settings. does not call check() on each member, you have to do that separately. */
        void checkRsConfig() const;

        /** check if modification makes sense */
        static Status legalChange(const ReplSetConfig& old, const ReplSetConfig& n);

        /**
         * 1. Checks the validity of member variables. (may uassert)
         * 2. Saves a BSON copy of the config to local.system.replset
         * 3. If 'comment' isn't empty and we're a primary or not yet initiated, log an 'n' op
         *    to the oplog.  This is important because it establishes our lastOpWritten time.
         */
        void saveConfigLocally(BSONObj comment); // to local db

        /**
         * Update members' groups when the config changes but members stay the same.
         */
        void updateMembers(List1<Member> &dest) const;

        BSONObj asBson() const;

        /**
         * Getter and setter for _majority. This is almost always
         * members.size()/2+1, but can be the number of non-arbiter members if
         * there are more arbiters than non-arbiters (writing to 3 out of 7
         * servers is safe if 4 of the servers are arbiters).
         */
    private:
        void setMajority();
    public:
        int getMajority() const;

        /**
         * Get the timeout to use for heartbeats.
         */
        int getHeartbeatTimeout() const;

        /**
         * Default timeout: 10 seconds
         */
        static const int DEFAULT_HB_TIMEOUT;

        /**
         * Returns if replication chaining is allowed.
         */
        bool chainingAllowed() const;

    private:
        ReplSetConfig();
        void init(const HostAndPort& h);
        void init(BSONObj cfg, bool force);

        /**
         * If replication can be chained. If chaining is disallowed, it can still be explicitly
         * enabled via the replSetSyncFrom command, but it will not happen automatically.
         */
        bool _chainingAllowed;
        int _majority;
        bool _ok;

        /**
         * This function takes a config BSON and converts it into actual Member objects
         * with the proper config filled in.  It then pushes all the new Members onto
         * the member variable "members".  It also does some config checks and might uassert.
         */
        void from(BSONObj);

        /**
         * Just sets "version" to -5 and "_ok" to false.
         */
        void clear();

        /**
         * The timeout to use for heartbeats
         */
        int _heartbeatTimeout;

        struct TagClause;
        /**
         * This is a logical grouping of servers.  It is pointed to by a set of
         * servers with a certain tag.
         *
         * For example, suppose servers A, B, and C have the tag "dc" : "nyc". If we
         * have a rule {"dc" : 2}, then we want A _or_ B _or_ C to have the
         * write for one of the "dc" critiria to be fulfilled, so all three will
         * point to this subgroup. When one of their oplog-tailing cursors is
         * updated, this subgroup is updated.
         */
        struct TagSubgroup : boost::noncopyable {
            ~TagSubgroup(); // never called; not defined
            TagSubgroup(const std::string& nm) : name(nm) { }
            const std::string name;
            OpTime last;
            std::vector<TagClause*> clauses;

            // this probably won't actually point to valid members after the
            // subgroup is created, as initFromConfig() makes a copy of the
            // config
            std::set<MemberCfg*> m;

            void updateLast(const OpTime& op);

            //string toString() const;

            /**
             * If two tags have the same name, they should compare as equal so
             * that members don't have to update two identical groups on writes.
             */
            bool operator() (TagSubgroup& lhs, TagSubgroup& rhs) const {
                return lhs.name < rhs.name;
            }
        };

        /**
         * An argument in a rule.  For example, if we had the rule {dc : 2,
         * machines : 3}, "dc" : 2 and "machines" : 3 would be two TagClauses.
         *
         * Each tag clause has a set of associated subgroups.  For example, if
         * we had "dc" : 2, our subgroups might be "nyc", "sf", and "hk".
         */
        struct TagClause {
            OpTime last;
            std::map<std::string,TagSubgroup*> subgroups;
            TagRule *rule;
            std::string name;
            /**
             * If we have get a clause like {machines : 3} and this server is
             * tagged with "machines", then it's really {machines : 2}, as we
             * will always be up-to-date.  So, target would be 3 and
             * actualTarget would be 2, in that example.
             */
            int target;
            int actualTarget;

            void updateLast(const OpTime& op);
            std::string toString() const;
        };

        /**
         * Parses getLastErrorModes.
         */
        void parseRules(const BSONObj& modes);

        /**
         * Create a  hash containing every possible clause that could be used in a
         * rule and the servers related to that clause.
         *
         * For example, suppose we have the following servers:
         * A {"dc" : "ny", "ny" : "rk1"}
         * B {"dc" : "ny", "ny" : "rk1"}
         * C {"dc" : "ny", "ny" : "rk2"}
         * D {"dc" : "sf", "sf" : "rk1"}
         * E {"dc" : "sf", "sf" : "rk2"}
         *
         * This would give us the possible criteria:
         * "dc" -> {A, B, C},{D, E}
         * "ny" -> {A, B},{C}
         * "sf" -> {D},{E}
         */
        void _populateTagMap(std::map<std::string,TagClause> &tagMap);

    public:
        struct TagRule {
            std::vector<TagClause*> clauses;
            OpTime last;

            void updateLast(const OpTime& op);
            std::string toString() const;
        };
    };

} // namespace repl
} // namespace mongo
