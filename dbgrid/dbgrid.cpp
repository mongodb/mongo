// dbgrid.cpp

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
#include "../util/unittest.h"

const char *curNs = "";
Client *client = 0;

/* this is a good place to set a breakpoint when debugging, as lots of warning things
   (assert, wassert) call it.
*/
void sayDbContext(const char *errmsg) { 
	if( errmsg )
		problem() << errmsg << endl;
	printStackTrace();
}

#if !defined(_WIN32)

#include <signal.h>

void pipeSigHandler( int signal ) {
  psignal( signal, "Signal Received : ");
}

#else
void setupSignals() {}
#endif

void usage() { 
    cout << "Mongo dbgrid usage:\n";
    cout << " --port <portno>\n";
    cout << endl;
}

int port = 0;

void start() { 
}

int main(int argc, char* argv[], char *envp[] ) { 
#if !defined(_WIN32)
    signal(SIGPIPE, pipeSigHandler);
#endif

    for (int i = 1; i < argc; i++)  {
        if( argv[i] == 0 ) continue;
        string s = argv[i];
        if( s == "--port" ) { 
            port = atoi(argv[++i]);
        }
    }

    bool ok = port != 0;

    if( !ok ) {
        usage();
        return 1;
    }

	log() << "dbgrid starting" << endl;
	UnitTest::runTests();
    start();
	dbexit(0);
	return 0;
}

#undef exit
void dbexit(int rc, const char *why) { 
	log() << "dbexit: " << why << " rc:" << rc << endl;
	exit(rc);
}
