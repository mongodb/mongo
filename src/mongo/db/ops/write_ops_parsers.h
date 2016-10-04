/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/util/net/message.h"

namespace mongo {

/**
 * This file contains functions to parse write operations from the user-facing wire format using
 * either write commands or the legacy OP_INSERT/OP_UPDATE/OP_DELETE wire operations. Parse errors
 * are reported by uasserting.
 *
 * These only parse and validate the operation structure. No attempt is made to parse or validate
 * the objects to insert, or update and query operators.
 */

InsertOp parseInsertCommand(StringData dbName, const BSONObj& cmd);
UpdateOp parseUpdateCommand(StringData dbName, const BSONObj& cmd);
DeleteOp parseDeleteCommand(StringData dbName, const BSONObj& cmd);

InsertOp parseLegacyInsert(const Message& msg);
UpdateOp parseLegacyUpdate(const Message& msg);
DeleteOp parseLegacyDelete(const Message& msg);

}  // namespace mongo
