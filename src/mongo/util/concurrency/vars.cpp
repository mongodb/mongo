// vars.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "mutex.h"
#include "value.h"

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
        if( logLevel>=1 && followers.size() ) {
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
