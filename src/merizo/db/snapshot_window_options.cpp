/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "merizo/platform/basic.h"

#include "merizo/db/snapshot_window_options.h"

#include "merizo/platform/compiler.h"
#include "merizo/util/options_parser/startup_option_init.h"

namespace merizo {

SnapshotWindowParams snapshotWindowParams;

/**
 * After startup parameters have been initialized, set targetSnapshotHistoryWindowInSeconds to the
 * value of maxTargetSnapshotHistoryWindowInSeconds, in case the max has been altered. The cache
 * pressure is zero to begin with, so the user should not have to wait for the target to slowly
 * adjust to max.
 */
MERIZO_STARTUP_OPTIONS_POST(SetTargetSnapshotWindowSize)
(InitializerContext* context) {
    snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.store(
        snapshotWindowParams.maxTargetSnapshotHistoryWindowInSeconds.load());
    return Status::OK();
}

}  // namespace merizo
