// rs_config.cpp

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

#include "mongo/pch.h"

#include <boost/algorithm/string.hpp>

#include "mongo/db/dbhelpers.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/text.h"

using namespace bson;

namespace mongo {

    mongo::mutex ReplSetConfig::groupMx("RS tag group");
    const int ReplSetConfig::DEFAULT_HB_TIMEOUT = 10;

    static AtomicUInt _warnedAboutVotes = 0;
    void logOpInitiate(const bo&);

    void assertOnlyHas(BSONObj o, const set<string>& fields) {
        BSONObj::iterator i(o);
        while( i.more() ) {
            BSONElement e = i.next();
            if( !fields.count( e.fieldName() ) ) {
                uasserted(13434, str::stream() << "unexpected field '" << e.fieldName() << "' in object");
            }
        }
    }

    list<HostAndPort> ReplSetConfig::otherMemberHostnames() const {
        list<HostAndPort> L;
        for( vector<MemberCfg>::const_iterator i = members.begin(); i != members.end(); i++ ) {
            if( !i->h.isSelf() )
                L.push_back(i->h);
        }
        return L;
    }

    /* comment MUST only be set when initiating the set by the initiator */
    void ReplSetConfig::saveConfigLocally(bo comment) {
        checkRsConfig();
        log() << "replSet info saving a newer config version to local.system.replset" << rsLog;
        {
            Lock::GlobalWrite lk; // TODO: does this really need to be a global lock?
            Client::Context cx( rsConfigNs );
            cx.db()->flushFiles(true);

            //theReplSet->lastOpTimeWritten = ??;
            //rather than above, do a logOp()? probably
            BSONObj o = asBson();
            Helpers::putSingletonGod(rsConfigNs.c_str(), o, false/*logOp=false; local db so would work regardless...*/);
            if( !comment.isEmpty() && (!theReplSet || theReplSet->isPrimary()) )
                logOpInitiate(comment);

            cx.db()->flushFiles(true);
        }
        log() << "replSet saveConfigLocally done" << rsLog;
    }

    bo ReplSetConfig::MemberCfg::asBson() const {
        bob b;
        b << "_id" << _id;
        b.append("host", h.toString());
        if( votes != 1 ) b << "votes" << votes;
        if( priority != 1.0 ) b << "priority" << priority;
        if( arbiterOnly ) b << "arbiterOnly" << true;
        if( slaveDelay ) b << "slaveDelay" << slaveDelay;
        if( hidden ) b << "hidden" << hidden;
        if( !buildIndexes ) b << "buildIndexes" << buildIndexes;
        if( !tags.empty() ) {
            BSONObjBuilder a;
            for( map<string,string>::const_iterator i = tags.begin(); i != tags.end(); i++ )
                a.append((*i).first, (*i).second);
            b.append("tags", a.done());
        }
        return b.obj();
    }

    void ReplSetConfig::updateMembers(List1<Member> &dest) const {
        for (vector<MemberCfg>::const_iterator source = members.begin(); source < members.end(); source++) {
            for( Member *d = dest.head(); d; d = d->next() ) {
                if (d->fullName() == (*source).h.toString()) {
                    scoped_lock lk(groupMx);
                    d->configw().groupsw() = (*source).groups();
                }
            }
        }
    }

    bo ReplSetConfig::asBson() const {
        bob b;
        b.append("_id", _id).append("version", version);

        BSONArrayBuilder a;
        for( unsigned i = 0; i < members.size(); i++ )
            a.append( members[i].asBson() );
        b.append("members", a.arr());

        BSONObjBuilder settings;
        bool empty = true;

        if (!rules.empty()) {
            bob modes;
            for (map<string,TagRule*>::const_iterator it = rules.begin(); it != rules.end(); it++) {
                bob clauses;
                vector<TagClause*> r = (*it).second->clauses;
                for (vector<TagClause*>::iterator it2 = r.begin(); it2 < r.end(); it2++) {
                    clauses << (*it2)->name << (*it2)->target;
                }
                modes << (*it).first << clauses.obj();
            }
            settings << "getLastErrorModes" << modes.obj();
            empty = false;
        }

        if (!getLastErrorDefaults.isEmpty()) {
            settings << "getLastErrorDefaults" << getLastErrorDefaults;
            empty = false;
        }

        if (_heartbeatTimeout != DEFAULT_HB_TIMEOUT) {
            settings << "heartbeatTimeoutSecs" << _heartbeatTimeout;
            empty = false;
        }

        if (!_chainingAllowed) {
            settings << "chainingAllowed" << _chainingAllowed;
            empty = false;
        }

        if (!empty) {
            b << "settings" << settings.obj();
        }

        return b.obj();
    }

    static inline void mchk(bool expr) {
        uassert(13126, "bad Member config", expr);
    }

    void ReplSetConfig::MemberCfg::check() const {
        mchk(_id >= 0 && _id <= 255);
        mchk(priority >= 0 && priority <= 1000);
        mchk(votes <= 100); // votes >= 0 because it is unsigned
        uassert(13419, "priorities must be between 0.0 and 1000", priority >= 0.0 && priority <= 1000);
        uassert(13437, "slaveDelay requires priority be zero", slaveDelay == 0 || priority == 0);
        uassert(13438, "bad slaveDelay value", slaveDelay >= 0 && slaveDelay <= 3600 * 24 * 366);
        uassert(13439, "priority must be 0 when hidden=true", priority == 0 || !hidden);
        uassert(13477, "priority must be 0 when buildIndexes=false", buildIndexes || priority == 0);
    }
/*
    string ReplSetConfig::TagSubgroup::toString() const {
        bool first = true;
        string result = "\""+name+"\": [";
        for (set<const MemberCfg*>::const_iterator i = m.begin(); i != m.end(); i++) {
            if (!first) {
                result += ", ";
            }
            first = false;
            result += (*i)->h.toString();
        }
        return result+"]";
    }
    */
    string ReplSetConfig::TagClause::toString() const {
        string result = name+": {";
        for (map<string,TagSubgroup*>::const_iterator i = subgroups.begin(); i != subgroups.end(); i++) {
//TEMP?            result += (*i).second->toString()+", ";
        }
        result += "TagClause toString TEMPORARILY DISABLED";
        return result + "}";
    }

    string ReplSetConfig::TagRule::toString() const {
        string result = "{";
        for (vector<TagClause*>::const_iterator it = clauses.begin(); it < clauses.end(); it++) {
            result += ((TagClause*)(*it))->toString()+",";
        }
        return result+"}";
    }

    void ReplSetConfig::TagSubgroup::updateLast(const OpTime& op) {
        RACECHECK
        if (last < op) {
            last = op;

            for (vector<TagClause*>::iterator it = clauses.begin(); it < clauses.end(); it++) {
                (*it)->updateLast(op);
            }
        }
    }

    void ReplSetConfig::TagClause::updateLast(const OpTime& op) {
        RACECHECK
        if (last >= op) {
            return;
        }

        // check at least n subgroups greater than clause.last
        int count = 0;
        map<string,TagSubgroup*>::iterator it;
        for (it = subgroups.begin(); it != subgroups.end(); it++) {
            if ((*it).second->last >= op) {
                count++;
            }
        }

        if (count >= actualTarget) {
            last = op;
            rule->updateLast(op);
        }
    }

    void ReplSetConfig::TagRule::updateLast(const OpTime& op) {
        OpTime *earliest = (OpTime*)&op;
        vector<TagClause*>::iterator it;

        for (it = clauses.begin(); it < clauses.end(); it++) {
            if ((*it)->last < *earliest) {
                earliest = &(*it)->last;
            }
        }

        // rules are simply and-ed clauses, so whatever the most-behind
        // clause is at is what the rule is at
        last = *earliest;
    }

    /** @param o old config
        @param n new config
        */
    /*static*/
    bool ReplSetConfig::legalChange(const ReplSetConfig& o, const ReplSetConfig& n, string& errmsg) {
        verify( theReplSet );

        if( o._id != n._id ) {
            errmsg = "set name may not change";
            return false;
        }
        /* TODO : wonder if we need to allow o.version < n.version only, which is more lenient.
                  if someone had some intermediate config this node doesnt have, that could be
                  necessary.  but then how did we become primary?  so perhaps we are fine as-is.
                  */
        if( o.version >= n.version ) {
            errmsg = str::stream() << "version number must increase, old: "
                                   << o.version << " new: " << n.version;
            return false;
        }

        map<HostAndPort,const ReplSetConfig::MemberCfg*> old;
        bool isLocalHost = false;
        for( vector<ReplSetConfig::MemberCfg>::const_iterator i = o.members.begin(); i != o.members.end(); i++ ) {
            if (i->h.isLocalHost()) {
                isLocalHost = true;
            }
            old[i->h] = &(*i);
        }
        int me = 0;
        for( vector<ReplSetConfig::MemberCfg>::const_iterator i = n.members.begin(); i != n.members.end(); i++ ) {
            const ReplSetConfig::MemberCfg& m = *i;
            if ( (isLocalHost && !m.h.isLocalHost()) || (!isLocalHost && m.h.isLocalHost())) {
                log() << "reconfig error, cannot switch between localhost and hostnames: "
                      << m.h.toString() << rsLog;
                uasserted(13645, "hosts cannot switch between localhost and hostname");
            }
            if( old.count(m.h) ) {
                const ReplSetConfig::MemberCfg& oldCfg = *old[m.h];
                if( oldCfg._id != m._id ) {
                    log() << "replSet reconfig error with member: " << m.h.toString() << rsLog;
                    uasserted(13432, "_id may not change for members");
                }
                if( oldCfg.buildIndexes != m.buildIndexes ) {
                    log() << "replSet reconfig error with member: " << m.h.toString() << rsLog;
                    uasserted(13476, "buildIndexes may not change for members");
                }
                /* are transitions to and from arbiterOnly guaranteed safe?  if not, we should disallow here.
                   there is a test at replsets/replsetarb3.js */
                if( oldCfg.arbiterOnly != m.arbiterOnly ) {
                    log() << "replSet reconfig error with member: " << m.h.toString() << " arbiterOnly cannot change. remove and readd the member instead " << rsLog;
                    uasserted(13510, "arbiterOnly may not change for members");
                }
            }
            if( m.h.isSelf() )
                me++;
        }

        uassert(13433, "can't find self in new replset config", me == 1);

        return true;
    }

    void ReplSetConfig::clear() {
        version = -5;
        _ok = false;
    }

    void ReplSetConfig::setMajority() {
        int total = members.size();
        int nonArbiters = total;
        int strictMajority = total/2+1;

        for (vector<MemberCfg>::iterator it = members.begin(); it < members.end(); it++) {
            if ((*it).arbiterOnly) {
                nonArbiters--;
            }
        }

        // majority should be all "normal" members if we have something like 4
        // arbiters & 3 normal members
        _majority = (strictMajority > nonArbiters) ? nonArbiters : strictMajority;
    }

    int ReplSetConfig::getMajority() const {
        return _majority;
    }

    void ReplSetConfig::checkRsConfig() const {
        uassert(13132,
                str::stream() << "nonmatching repl set name in _id field: " << _id << " vs. " << cmdLine.ourSetName(),
                _id == cmdLine.ourSetName());
        uassert(13308, "replSet bad config version #", version > 0);
        uassert(13133, "replSet bad config no members", members.size() >= 1);
        uassert(13309, "replSet bad config maximum number of members is 12", members.size() <= 12);
        {
            unsigned voters = 0;
            for( vector<MemberCfg>::const_iterator i = members.begin(); i != members.end(); ++i ) {
                if( i->votes )
                    voters++;
            }
            uassert(13612, "replSet bad config maximum number of voting members is 7", voters <= 7);
            uassert(13613, "replSet bad config no voting members", voters > 0);
        }
    }

    void ReplSetConfig::_populateTagMap(map<string,TagClause> &tagMap) {
        // create subgroups for each server corresponding to each of
        // its tags. E.g.:
        //
        // A is tagged with {"server" : "A", "dc" : "ny"}
        // B is tagged with {"server" : "B", "dc" : "ny"}
        //
        // At the end of this step, tagMap will contain:
        //
        // "server" => {"A" : [A], "B" : [B]}
        // "dc" => {"ny" : [A,B]}

        for (unsigned i=0; i<members.size(); i++) {
            MemberCfg member = members[i];

            for (map<string,string>::iterator tag = member.tags.begin(); tag != member.tags.end(); tag++) {
                string label = (*tag).first;
                string value = (*tag).second;

                TagClause& clause = tagMap[label];
                clause.name = label;

                TagSubgroup* subgroup;
                // search for "ny" in "dc"'s clause
                if (clause.subgroups.find(value) == clause.subgroups.end()) {
                    clause.subgroups[value] = subgroup = new TagSubgroup(value);
                }
                else {
                    subgroup = clause.subgroups[value];
                }

                subgroup->m.insert(&members[i]);
            }
        }
    }

    void ReplSetConfig::parseRules(const BSONObj& modes) {
        map<string,TagClause> tagMap;
        _populateTagMap(tagMap);

        for (BSONObj::iterator i = modes.begin(); i.more(); ) {
            unsigned int primaryOnly = 0;

            // ruleName : {dc : 2, m : 3}
            BSONElement rule = i.next();
            uassert(14046, "getLastErrorMode rules must be objects", rule.type() == mongo::Object);

            TagRule* r = new TagRule();

            BSONObj clauseObj = rule.Obj();
            for (BSONObj::iterator c = clauseObj.begin(); c.more(); ) {
                BSONElement clauseElem = c.next();
                uassert(14829, "getLastErrorMode criteria must be numeric", clauseElem.isNumber());

                // get the clause, e.g., "x.y" : 3
                const char *criteria = clauseElem.fieldName();
                int value = clauseElem.numberInt();
                uassert(14828, str::stream() << "getLastErrorMode criteria must be greater than 0: " << clauseElem, value > 0);

                TagClause* node = new TagClause(tagMap[criteria]);

                int numGroups = node->subgroups.size();
                uassert(14831, str::stream() << "mode " << clauseObj << " requires "
                        << value << " tagged with " << criteria << ", but only "
                        << numGroups << " with this tag were found", numGroups >= value);

                node->name = criteria;
                node->target = value;
                // if any subgroups contain "me", we can decrease the target
                node->actualTarget = node->target;

                // then we want to add pointers between clause & subgroup
                for (map<string,TagSubgroup*>::iterator sgs = node->subgroups.begin();
                     sgs != node->subgroups.end(); sgs++) {
                    bool foundMe = false;
                    (*sgs).second->clauses.push_back(node);

                    // if this subgroup contains the primary, it's automatically always up-to-date
                    for( set<MemberCfg*>::const_iterator cfg = (*sgs).second->m.begin();
                         cfg != (*sgs).second->m.end(); 
                         cfg++) 
                    {
                        if ((*cfg)->h.isSelf()) {
                            node->actualTarget--;
                            foundMe = true;
                        }
                    }

                    scoped_lock lk(groupMx);
                    for (set<MemberCfg *>::iterator cfg = (*sgs).second->m.begin();
                         !foundMe && cfg != (*sgs).second->m.end(); cfg++) {
                        (*cfg)->groupsw().insert((*sgs).second);
                    }
                }

                // if all of the members of this clause involve the primary, it's always up-to-date
                if (node->actualTarget == 0) {
                    node->last = OpTime(INT_MAX, INT_MAX);
                    primaryOnly++;
                }

                // this is a valid clause, so we want to add it to its rule
                node->rule = r;
                r->clauses.push_back(node);
            }

            // if all of the clauses are satisfied by the primary, this rule is trivially true
            if (primaryOnly == r->clauses.size()) {
                r->last = OpTime(INT_MAX, INT_MAX);
            }

            // if we got here, this is a valid rule
            LOG(1) << "replSet new rule " << rule.fieldName() << ": " << r->toString() << rsLog;
            rules[rule.fieldName()] = r;
        }
    }

    void ReplSetConfig::from(BSONObj o) {
        static const string legal[] = {"_id","version", "members","settings"};
        static const set<string> legals(legal, legal + 4);
        assertOnlyHas(o, legals);

        md5 = o.md5();
        _id = o["_id"].String();
        if( o["version"].ok() ) {
            version = o["version"].numberInt();
            uassert(13115, "bad " + rsConfigNs + " config: version", version > 0);
        }

        set<string> hosts;
        set<int> ords;
        vector<BSONElement> members;
        try {
            members = o["members"].Array();
        }
        catch(...) {
            uasserted(13131, "replSet error parsing (or missing) 'members' field in config object");
        }

        unsigned localhosts = 0;
        for( unsigned i = 0; i < members.size(); i++ ) {
            BSONObj mobj = members[i].Obj();
            MemberCfg m;
            try {
                static const string legal[] = {
                    "_id","votes","priority","host", "hidden","slaveDelay",
                    "arbiterOnly","buildIndexes","tags","initialSync" // deprecated
                };
                static const set<string> legals(legal, legal + 10);
                assertOnlyHas(mobj, legals);

                try {
                    m._id = (int) mobj["_id"].Number();
                }
                catch(...) {
                    /* TODO: use of string exceptions may be problematic for reconfig case! */
                    throw "_id must be numeric";
                }
                try {
                    string s = mobj["host"].String();
                    boost::trim(s);
                    m.h = HostAndPort(s);
                    if ( !m.h.hasPort() ) {
                        // make port explicit even if default 
                        m.h.setPort(m.h.port());
                    }
                }
                catch(...) {
                    throw string("bad or missing host field? ") + mobj.toString();
                }
                if( m.h.isLocalHost() )
                    localhosts++;
                m.arbiterOnly = mobj["arbiterOnly"].trueValue();
                m.slaveDelay = mobj["slaveDelay"].numberInt();
                if( mobj.hasElement("hidden") )
                    m.hidden = mobj["hidden"].trueValue();
                if( mobj.hasElement("buildIndexes") )
                    m.buildIndexes = mobj["buildIndexes"].trueValue();
                if( mobj.hasElement("priority") )
                    m.priority = mobj["priority"].Number();
                if( mobj.hasElement("votes") )
                    m.votes = (unsigned) mobj["votes"].Number();
                if (m.votes > 1 && !_warnedAboutVotes) {
                    log() << "\t\tWARNING: Having more than 1 vote on a single replicaset member is"
                          << startupWarningsLog;
                    log() << "\t\tdeprecated, as it causes issues with majority write concern. For"
                          << startupWarningsLog;
                    log() << "\t\tmore information, see "
                          << "http://dochub.mongodb.org/core/replica-set-votes-deprecated"
                          << startupWarningsLog;
                    _warnedAboutVotes.set(1);
                }
                if( mobj.hasElement("tags") ) {
                    const BSONObj &t = mobj["tags"].Obj();
                    for (BSONObj::iterator c = t.begin(); c.more(); c.next()) {
                        m.tags[(*c).fieldName()] = (*c).String();
                    }
                    uassert(14827, "arbiters cannot have tags", !m.arbiterOnly || m.tags.empty() );
                }
                m.check();
            }
            catch( const char * p ) {
                log() << "replSet cfg parsing exception for members[" << i << "] " << p << rsLog;
                stringstream ss;
                ss << "replSet members[" << i << "] " << p;
                uassert(13107, ss.str(), false);
            }
            catch(DBException& e) {
                log() << "replSet cfg parsing exception for members[" << i << "] " << e.what() << rsLog;
                stringstream ss;
                ss << "bad config for member[" << i << "] " << e.what();
                uassert(13135, ss.str(), false);
            }
            if( !(ords.count(m._id) == 0 && hosts.count(m.h.toString()) == 0) ) {
                log() << "replSet " << o.toString() << rsLog;
                uassert(13108, "bad replset config -- duplicate hosts in the config object?", false);
            }
            hosts.insert(m.h.toString());
            ords.insert(m._id);
            this->members.push_back(m);
        }
        uassert(13393, "can't use localhost in repl set member names except when using it for all members", localhosts == 0 || localhosts == members.size());
        uassert(13117, "bad " + rsConfigNs + " config", !_id.empty());

        if( o["settings"].ok() ) {
            BSONObj settings = o["settings"].Obj();
            if( settings["getLastErrorModes"].ok() ) {
                parseRules(settings["getLastErrorModes"].Obj());
            }
            ho.check();
            try { getLastErrorDefaults = settings["getLastErrorDefaults"].Obj().copy(); }
            catch(...) { }

            if (settings.hasField("heartbeatTimeoutSecs")) {
                int timeout = settings["heartbeatTimeoutSecs"].numberInt();
                uassert(16438, "Heartbeat timeout must be non-negative", timeout >= 0);
                _heartbeatTimeout = timeout;
            }

            // If the config explicitly sets chaining to false, turn it off.
            if (settings.hasField("chainingAllowed") &&
                !settings["chainingAllowed"].trueValue()) {
                _chainingAllowed = false;
            }
        }

        // figure out the majority for this config
        setMajority();
    }

    bool ReplSetConfig::chainingAllowed() const {
        return _chainingAllowed;
    }

    int ReplSetConfig::getHeartbeatTimeout() const {
        return _heartbeatTimeout;
    }

    static inline void configAssert(bool expr) {
        uassert(13122, "bad repl set config?", expr);
    }

    ReplSetConfig::ReplSetConfig() :
        version(EMPTYCONFIG),
        _chainingAllowed(true),
        _majority(-1),
        _ok(false),
        _heartbeatTimeout(DEFAULT_HB_TIMEOUT) {
    }

    ReplSetConfig* ReplSetConfig::make(BSONObj cfg, bool force) {
        auto_ptr<ReplSetConfig> ret(new ReplSetConfig());
        ret->init(cfg, force);
        return ret.release();
    }

    void ReplSetConfig::init(BSONObj cfg, bool force) {
        _constructed = false;
        clear();
        from(cfg);
        if( force ) {
            version += rand() % 100000 + 10000;
        }
        configAssert( version < 0 /*unspecified*/ || (version >= 1) );
        if( version < 1 )
            version = 1;
        _ok = true;
        _constructed = true;
    }

    ReplSetConfig* ReplSetConfig::make(const HostAndPort& h) {
        auto_ptr<ReplSetConfig> ret(new ReplSetConfig());
        ret->init(h);
        return ret.release();
    }

    ReplSetConfig* ReplSetConfig::makeDirect() {
        DBDirectClient cli;
        BSONObj config = cli.findOne(rsConfigNs, Query()).getOwned();

        // Check for no local config
        if (config.isEmpty()) {
            return new ReplSetConfig();
        }

        return make(config, false);
    }

    void ReplSetConfig::init(const HostAndPort& h) {
        LOG(2) << "ReplSetConfig load " << h.toString() << rsLog;

        _constructed = false;
        clear();
        int level = 2;
        DEV level = 0;

        BSONObj cfg;
        int v = -5;
        try {
            if( h.isSelf() ) {
                ;
            }
            else {
                /* first, make sure other node is configured to be a replset. just to be safe. */
                string setname = cmdLine.ourSetName();
                BSONObj cmd = BSON( "replSetHeartbeat" << setname );
                int theirVersion;
                BSONObj info;
                log() << "trying to contact " << h.toString() << rsLog;
                bool ok = requestHeartbeat(setname, "", h.toString(), info, -2, theirVersion);
                if( info["rs"].trueValue() ) {
                    // yes, it is a replicate set, although perhaps not yet initialized
                }
                else {
                    if( !ok ) {
                        log() << "replSet TEMP !ok heartbeating " << h.toString() << " on cfg load" << rsLog;
                        if( !info.isEmpty() )
                            log() << "replSet info " << h.toString() << " : " << info.toString() << rsLog;
                        return;
                    }
                    {
                        stringstream ss;
                        ss << "replSet error: member " << h.toString() << " is not in --replSet mode";
                        msgassertedNoTrace(13260, ss.str().c_str()); // not caught as not a user exception - we want it not caught
                        //for python err# checker: uassert(13260, "", false);
                    }
                }
            }

            v = -4;
            unsigned long long count = 0;
            try {
                ScopedConn conn(h.toString());
                v = -3;
                cfg = conn.findOne(rsConfigNs, Query()).getOwned();
                count = conn.count(rsConfigNs);
            }
            catch ( DBException& ) {
                if ( !h.isSelf() ) {
                    throw;
                }

                // on startup, socket is not listening yet
                DBDirectClient cli;
                cfg = cli.findOne( rsConfigNs, Query() ).getOwned();
                count = cli.count(rsConfigNs);
            }

            if( count > 1 )
                uasserted(13109, str::stream() << "multiple rows in " << rsConfigNs << " not supported host: " << h.toString());

            if( cfg.isEmpty() ) {
                version = EMPTYCONFIG;
                return;
            }
            version = -1;
        }
        catch( DBException& e) {
            version = v;
            LOG(level) << "replSet load config couldn't get from " << h.toString() << ' ' << e.what() << rsLog;
            return;
        }

        from(cfg);
        checkRsConfig();
        _ok = true;
        LOG(level) << "replSet load config ok from " << (h.isSelf() ? "self" : h.toString()) << rsLog;
        _constructed = true;
    }

}
