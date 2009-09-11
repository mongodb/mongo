// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

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

#pragma once

namespace mongo {

#define NOMINMAX

#if defined(_WIN32)
    const bool debug=true;
#else
    const bool debug=false;
#endif

    // pdfile versions
    const int VERSION = 4;
    const int VERSION_MINOR = 5;
    
    // mongo version
    extern const char versionString[];
    
    enum ExitCode {
        EXIT_CLEAN = 0 , 
        EXIT_BADOPTIONS = 2 , 
        EXIT_REPLICATION_ERROR = 3 ,
        EXIT_NEED_UPGRADE = 4 ,
        EXIT_KILL = 12 ,
        EXIT_ABRUBT = 14 ,
        EXIT_NTSERVICE_ERROR = 20 ,
        EXIT_JAVA = 21 ,
        EXIT_OOM_MALLOC = 42 , 
        EXIT_OOM_REALLOC = 43 , 
        EXIT_FS = 45 ,
        EXIT_POSSIBLE_CORRUPTION = 60 , // this means we detected a possible corruption situation, like a buf overflow
        EXIT_UNCAUGHT = 100 , // top level exception that wasn't caught
        EXIT_TEST = 101 ,

    };

    void dbexit( ExitCode returnCode, const char *whyMsg = "");

    /**
       this is here so you can't just type exit() to quit the program
       you should either use dbexit to shutdown cleanly, or ::exit to tell the system to quiy
       if you use this, you'll get a link error since mongo::exit isn't defined
     */
    void exit( ExitCode returnCode );
    bool inShutdown();
    
} // namespace mongo

#include <memory>
#include <string>
#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <sstream>
#include <signal.h>

#include "targetver.h"
#include "time.h"
#include "string.h"
#include "limits.h"

using namespace std;

#undef yassert
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/smart_ptr.hpp>
#define BOOST_SPIRIT_THREADSAFE

#include <boost/version.hpp>

#if BOOST_VERSION >= 103800
#define BOOST_SPIRIT_USE_OLD_NAMESPACE
#include <boost/spirit/include/classic_core.hpp>
#include <boost/spirit/include/classic_loops.hpp>
#else
#include <boost/spirit/core.hpp>
#include <boost/spirit/utility/loops.hpp>
#endif

#include <boost/tuple/tuple.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/condition.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/xtime.hpp>
#undef assert
#define assert xassert
#define yassert 1
using namespace boost::filesystem;

#include "util/debug_util.h"
#include "util/goodies.h"
#include "util/log.h"
#include "util/allocator.h"
#include "util/assert_util.h"

namespace mongo {

    void sayDbContext(const char *msg = 0);
    void rawOut( const string &s );

} // namespace mongo

namespace mongo {

    const char * gitVersion();
    const char * sysInfo();
    string mongodVersion();
    
    void printGitVersion();
    void printSysInfo();

    typedef char _TCHAR;

#define null (0)

} // namespace mongo
