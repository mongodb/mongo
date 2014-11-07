// /db/repl/rs.h

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

#include "mongo/bson/oid.h"
#include "mongo/bson/optime.h"
#include "mongo/db/commands.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/repl/consensus.h"
#include "mongo/db/repl/manager.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_set.h"
#include "mongo/db/repl/repl_set_impl.h"
#include "mongo/db/repl/rs_base.h"
#include "mongo/db/repl/rs_config.h"
#include "mongo/db/repl/rs_exception.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/server.h"
#include "mongo/db/repl/state_box.h"
#include "mongo/db/repl/sync_source_feedback.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/value.h"
#include "mongo/util/net/hostandport.h"

/**
 * Order of Events
 *
 * On startup, if the --replSet option is present, startReplSets is called.
 * startReplSets forks off a new thread for replica set activities.  It creates
 * the global theReplSet variable and calls go() on it.
 *
 * theReplSet's constructor changes the replica set's state to RS_STARTUP,
 * starts the replica set manager, and loads the config (if the replica set
 * has been initialized).
 */

namespace mongo {
namespace repl {

    extern class ReplSet *theReplSet; // null until initialized

    class ReplSetSeedList;

    // Main entry point for replica sets
    void startReplSets(ReplSetSeedList *replSetSeedList);

} // namespace repl
} // namespace mongo
