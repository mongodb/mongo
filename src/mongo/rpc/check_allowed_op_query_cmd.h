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

namespace mongo {
/**
 * Logs a warning message and throws if 'cmd' is not an allowed 'OP_QUERY' command.
 *
 * OP_QUERY commands are no longer serviced by mongos or mongod processes, with the following
 * exceptions.
 *
 * - The isMaster/ismaster/hello OP_QUERY commands are allowed for connection handshake.
 *
 * - The _isSelf/saslContinue/saslStart OP_QUERY commands are allowed for intra-cluster
 * communication in a mixed version cluster. V5.0 nodes may send those commands as OP_QUERY ones.
 */
void checkAllowedOpQueryCommand(Client& client, StringData cmd);

}  // namespace mongo
