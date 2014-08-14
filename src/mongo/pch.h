   /** @file pch.h : include file for standard system include files,
 *  or project specific include files that are used frequently, but
 *  are changed infrequently
 */

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

#ifndef MONGO_PCH_H
#define MONGO_PCH_H

// our #define macros must not be active when we include
// system headers and boost headers
#include "mongo/client/undef_macros.h"

#include "mongo/platform/basic.h"

#include <ctime>
#include <cstring>
#include <string>
#include <memory>
#include <string>
#include <iostream>
#include <map>
#include <vector>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include <time.h>
#include <string.h>
#include <limits.h>


#define BOOST_FILESYSTEM_VERSION 3
#include <boost/shared_ptr.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/version.hpp>

#include "mongo/client/redef_macros.h"

#include "mongo/client/export_macros.h"

namespace mongo {

    using namespace std;
    using boost::shared_ptr;
}


#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/goodies.h"
#include "mongo/util/allocator.h"

#endif // MONGO_PCH_H
