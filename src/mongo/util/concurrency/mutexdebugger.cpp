// vars.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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
