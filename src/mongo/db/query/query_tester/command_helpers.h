// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/dbclient_connection.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo::query_tester {
// Returns the array element containing result documents.
BSONObj getResultsFromCommandResponse(const BSONObj& cmdResponse, size_t);

BSONObj runCommand(DBClientConnection* conn, const std::string& db, const BSONObj& commandToRun);

void runCommandAssertOK(DBClientConnection*,
                        const BSONObj& command,
                        const std::string& db,
                        std::vector<ErrorCodes::Error> acceptableErrorCodes = {});
}  // namespace mongo::query_tester
