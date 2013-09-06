// @file multicmd.h

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

#include "mongo/db/repl/connections.h"
#include "mongo/util/background.h"

namespace mongo {

    struct Target {
        Target(string hostport) : toHost(hostport), ok(false) { }
        //Target() : ok(false) { }
        const string toHost;
        bool ok;
        BSONObj result;
    };

    /** send a command to several servers in parallel.  waits for all to complete before 
        returning.  
        
        in: Target::toHost
        out: Target::result and Target::ok
    */
    void multiCommand(BSONObj cmd, list<Target>& L);

    class _MultiCommandJob : public BackgroundJob {
    public:
        BSONObj& cmd;
        Target& d;
        _MultiCommandJob(BSONObj& _cmd, Target& _d) : cmd(_cmd), d(_d) { }

    private:
        string name() const { return "MultiCommandJob"; }
        void run() {
            try {
                ScopedConn c(d.toHost);
                d.ok = c.runCommand("admin", cmd, d.result);
            }
            catch(DBException&) {
                DEV log() << "dev caught dbexception on multiCommand " << d.toHost << rsLog;
            }
        }
    };

    inline void multiCommand(BSONObj cmd, list<Target>& L) {
        list< shared_ptr<BackgroundJob> > jobs;

        for( list<Target>::iterator i = L.begin(); i != L.end(); i++ ) {
            Target& d = *i;
            _MultiCommandJob *j = new _MultiCommandJob(cmd, d);
            jobs.push_back( shared_ptr<BackgroundJob>(j) );
            j->go();
        }

        for( list< shared_ptr<BackgroundJob> >::iterator i = jobs.begin(); i != jobs.end(); i++ ) {
            (*i)->wait();
        }
    }
}
