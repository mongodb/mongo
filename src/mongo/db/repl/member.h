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

#include "mongo/db/repl/heartbeat_info.h"
#include "mongo/db/repl/rs_config.h"
#include "mongo/util/concurrency/list.h"

namespace mongo {
namespace replset {

    /* member of a replica set */
    class Member : public List1<Member>::Base {
    private:
        ~Member(); // intentionally unimplemented as should never be called -- see List1<>::Base.
        Member(const Member&); 
    public:
        Member(HostAndPort h, unsigned ord, const ReplSetConfig::MemberCfg *c, bool self) :
            _config(*c),
            _h(h),
            _hbinfo(ord) {
            verify(c);
            if( self )
                _hbinfo.health = 1.0;
        }


        string fullName() const { return h().toString(); }
        const ReplSetConfig::MemberCfg& config() const { return _config; }
        ReplSetConfig::MemberCfg& configw() { return _config; }
        const HeartbeatInfo& hbinfo() const { return _hbinfo; }
        HeartbeatInfo& get_hbinfo() { return _hbinfo; }
        string lhb() const { return _hbinfo.lastHeartbeatMsg; }
        MemberState state() const { return _hbinfo.hbstate; }
        const HostAndPort& h() const { return _h; }
        unsigned id() const { return _hbinfo.id(); }

        // not arbiter, not priority 0
        bool potentiallyHot() const { return _config.potentiallyHot(); }
        void summarizeMember(stringstream& s) const;
        // If we could sync from this member.  This doesn't tell us anything about the quality of
        // this member, just if they are a possible sync target.
        bool syncable() const;

    private:
        friend class ReplSetImpl;
        ReplSetConfig::MemberCfg _config;
        const HostAndPort _h;
        HeartbeatInfo _hbinfo;
    };

} // namespace replset
} // namespace mongo
