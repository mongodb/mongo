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

#include <iosfwd>

#include "mongo/db/repl/heartbeat_info.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

    /*
     * This class is a container for holding the transient heartbeat info for each member 
     * in the config.  It also holds a copy of the HostAndPort and allows quick access to it.
     * This class will be eliminated in the next few weeks to be replaced by a refactored
     * HeartbeatInfo class, so don't scrutinize too closely.
     */

    class NewMember {
    public:
        NewMember(HostAndPort hap, int id, int configIndex) :
            _configIndex(configIndex),
            _hap(hap),
            _hbinfo(id) {
        }

        string fullName() const { return hap().toString(); }
        int configIndex() const { return _configIndex; }
        const HeartbeatInfo& hbinfo() const { return _hbinfo; }
        HeartbeatInfo& get_hbinfo() { return _hbinfo; }
        const HostAndPort& hap() const { return _hap; }

    private:
        int _configIndex;
        HostAndPort _hap;
        HeartbeatInfo _hbinfo;
    };

} // namespace repl
} // namespace mongo
