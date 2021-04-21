/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/client.h"
#include "mongo/rpc/message.h"

namespace mongo {

/**
 * Logs a warning message for use of a legacy opcode or getLastError command once per each unique
 * client in 60 minutes-period by default.
 * - The specific deprecated op code or command is given by 'op' parameter.
 * - Each client is identified by remote IP if client metadata is not available. If client metadata
 *   is available, a client is identified by client metadata’s application name + driver name +
 *   driver version.
 * - The log message includes the following information.
 *   - Either client IP and port if client metadata is not available,
 *   - Or client metadata if client metadata is available.
 * - The warning period is defined by 'deprecatedWireOpsWarningPeriodInSeconds' setParameter.
 *   - The default value is 3600 seconds.
 *   - If this value is 0, every legacy op code or getLastError request is logged with a warning
 *     message.
 */
void warnDeprecation(Client& client, StringData op);

}  // namespace mongo
