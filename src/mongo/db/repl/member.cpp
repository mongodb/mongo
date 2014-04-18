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

#include "mongo/db/repl/member.h"

#include "mongo/db/repl/rs.h"
#include "mongo/util/mongoutils/html.h"


namespace mongo {

    using namespace mongoutils::html;

    void Member::summarizeMember(stringstream& s) const {
        s << tr();
        {
            stringstream u;
            u << "http://" << h().host() << ':' << (h().port() + 1000) << "/_replSet";
            s << td( a(u.str(), "", fullName()) );
        }
        s << td( id() );
        double h = hbinfo().health;
        bool ok = h > 0;
        s << td(red(str::stream() << h,h == 0));
        s << td(ago(hbinfo().upSince));
        bool never = false;
        {
            string h;
            time_t hb = hbinfo().lastHeartbeat;
            if( hb == 0 ) {
                h = "never";
                never = true;
            }
            else h = ago(hb) + " ago";
            s << td(h);
        }
        s << td(config().votes);
        s << td(config().priority);
        {
            string stateText = state().toString();
            if( _config.hidden )
                stateText += " (hidden)";
            if( ok || stateText.empty() )
                s << td(stateText); // text blank if we've never connected
            else
                s << td( grey(str::stream() << "(was " << state().toString() << ')', true) );
        }
        s << td( grey(hbinfo().lastHeartbeatMsg,!ok) );
        stringstream q;
        q << "/_replSetOplog?_id=" << id();
        s << td( a(q.str(), "", never ? "?" : hbinfo().opTime.toString()) );
        if( hbinfo().skew > INT_MIN ) {
            s << td( grey(str::stream() << hbinfo().skew,!ok) );
        }
        else
            s << td("");
        s << _tr();
    }

    bool Member::syncable() const {
        bool buildIndexes = theReplSet ? theReplSet->buildIndexes() : true;
        return hbinfo().up() && (config().buildIndexes || !buildIndexes) && state().readable();
    }

} // namespace mongo
