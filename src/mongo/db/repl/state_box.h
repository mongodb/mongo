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

#include "mongo/db/repl/health.h"

namespace mongo {

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
            verify(!sp.state.primary());
            sp.primary = remote;
        }
        StateBox() : m("StateBox") { }
    private:
        RWLock m;
        SP sp;
    };
} // namespace mongo
