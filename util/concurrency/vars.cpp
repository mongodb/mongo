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
#include "value.h"
#include "mutex.h"

namespace mongo { 

    mutex _atomicMutex("_atomicMutex");
    MutexDebugger mutexDebugger;

    void MutexDebugger::programEnding() { 
        if( followers.size() ) {
            std::cout << followers.size() << " mutexes in program" << endl;
            for( map< mid, set<mid> >::iterator i = followers.begin(); i != followers.end(); i++ ) { 
                cout << i->first << '\n';
                for( set<mid>::iterator j = i->second.begin(); j != i->second.end(); j++ )
                    cout << "  " << *j << '\n';
            }
            cout.flush();
        }
    }

}
