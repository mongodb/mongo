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
*/

#include "stdafx.h"
#include "goodies.h"
#include "background.h"

BackgroundJob *BackgroundJob::grab = 0;
boost::mutex BackgroundJob::mutex;

/* static */ 
void BackgroundJob::thr() { 
    assert( grab );
    assert( grab->state == NotStarted );
    grab->state = Running;
    grab->run();
    grab->state = Done;
    if( grab->deleteSelf )
        delete grab;
}

BackgroundJob& BackgroundJob::go() {
    boostlock bl(mutex);
    assert( grab == 0 );
    grab = this;
    boost::thread t(thr);
    while( grab )
        sleepmillis(2);
    return *this;
}

bool BackgroundJob::wait(int msMax) { 
    assert( state != NotStarted );
    int ms = 1;
    unsigned long long start = jsTime();
    while( state != Done ) { 
        sleepmillis(ms);
        if( ms < 1000 ) 
            ms = ms * 2;
        if( msMax && (jsTime()-start>msMax) )
            return false;
    }
    return true;
}
