/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
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

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/commands/profile_common.h"

namespace mongo {
namespace {

class ProfileCmd : public ProfileCmdBase {
public:
    ProfileCmd() = default;

    // On mongoS, the 'profile' command is only used to change the global 'slowms' and 'sampleRate'
    // parameters. Since it does not apply to any specific database but rather the mongoS as a
    // whole, we require that it be run on the 'admin' database.
    bool adminOnly() const final {
        return true;
    }

protected:
    int _applyProfilingLevel(OperationContext* opCtx,
                             const std::string& dbName,
                             int profilingLevel) const final {
        // Because mongoS does not allow profiling, but only uses the 'profile' command to change
        // 'slowms' and 'sampleRate' for logging purposes, we do not apply the profiling level here.
        // Instead, we validate that the user is not attempting to set a "real" profiling level.
        uassert(ErrorCodes::BadValue,
                "Profiling is not permitted on mongoS: the 'profile' field should be 0 to change "
                "'slowms' and 'sampleRate' settings for logging, or -1 to view current values",
                profilingLevel == -1 || profilingLevel == 0);

        return 0;
    }

} profileCmd;

}  // namespace
}  // namespace mongo
