// vars.cpp

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/pch.h"

#include "mongo/logger/logger.h"
#include "mongo/util/concurrency/mutex.h"


namespace mongo {

#if defined(_DEBUG)

    scoped_lock::PostStaticCheck::PostStaticCheck() {
        if ( StaticObserver::_destroyingStatics ) {
            cout << "_DEBUG warning trying to lock a mongo::mutex during static shutdown" << endl;
            //printStackTrace( cout ); // TODO: re-enable when we're ready to debug
        }
    }

    // intentional leak. otherwise destructor orders can be problematic at termination.
    MutexDebugger &mutexDebugger = *(new MutexDebugger());

    MutexDebugger::MutexDebugger() :
        x( *(new boost::mutex()) ), magic(0x12345678) {
        // optional way to debug lock order
        /*
        a = "a_lock";
        b = "b_lock";
        */
    }

    void MutexDebugger::programEnding() {
        using logger::LogSeverity;
        if(logger::globalLogDomain()->shouldLog(LogSeverity::Debug(1)) && followers.size()) {
            std::cout << followers.size() << " mutexes in program" << endl;
            for( map< mid, set<mid> >::iterator i = followers.begin(); i != followers.end(); i++ ) {
                cout << i->first;
                if( maxNest[i->first] > 1 )
                    cout << " maxNest:" << maxNest[i->first];
                cout << '\n';
                for( set<mid>::iterator j = i->second.begin(); j != i->second.end(); j++ )
                    cout << "  " << *j << '\n';
            }
            cout.flush();
        }
    }

#endif

}
