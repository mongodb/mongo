// debug_util.h

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

#pragma once

#include "mongo/config.h"

namespace mongo {

#if defined(MONGO_CONFIG_DEBUG_BUILD)
const bool kDebugBuild = true;
#else
const bool kDebugBuild = false;
#endif

#define MONGO_DEV if (kDebugBuild)
#define DEV MONGO_DEV

// The following declare one unique counter per enclosing function.
// NOTE The implementation double-increments on a match, but we don't really care.
#define MONGO_SOMETIMES(occasion, howOften) \
    for (static unsigned occasion = 0; ++occasion % howOften == 0;)
#define SOMETIMES MONGO_SOMETIMES

#define MONGO_OCCASIONALLY SOMETIMES(occasionally, 16)
#define OCCASIONALLY MONGO_OCCASIONALLY

#define MONGO_RARELY SOMETIMES(rarely, 128)
#define RARELY MONGO_RARELY

#define MONGO_ONCE for (static bool undone = true; undone; undone = false)
#define ONCE MONGO_ONCE

}  // namespace mongo
