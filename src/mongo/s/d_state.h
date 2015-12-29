/*
 *    Copyright (C) 2010 10gen Inc.
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

#include <string>

namespace mongo {

class BSONObj;
class Client;
class OperationContext;
class ShardedConnectionInfo;
class NamespaceString;

// -----------------
// --- core ---
// -----------------

/**
 * @return true if we have any shard info for the ns
 */
bool haveLocalShardingInfo(Client* client, const std::string& ns);

/**
 * Validates whether the shard chunk version for the specified collection is up to date and if
 * not, throws SendStaleConfigException.
 *
 * It is important (but not enforced) that method be called with the collection locked in at
 * least IS mode in order to ensure that the shard version won't change.
 *
 * @param ns Complete collection namespace to be cheched.
 */
void ensureShardVersionOKOrThrow(OperationContext* txn, const std::string& ns);

/**
 * If sharding is enabled, pass the insert along to MigrationSourceManager::logInsertOp
 * to determine whether the insert should be logged to the migration transfer mods log.
 *
 * 'ns' name of the collection in which the operation will occur.
 * 'obj' document being inserted.
 * 'notInActiveChunk' if true indicates that the insert is coming from a donor shard
 * in a current chunk migration, and so does not need to be entered in this shard's
 * outgoing transfer log.
 */
void logInsertOpForSharding(OperationContext* txn,
                            const char* ns,
                            const BSONObj& obj,
                            bool notInActiveChunk);

/**
 * If sharding is enabled, pass the update along to MigrationSourceManager::logUpdateOp
 * to determine whether the update should be logged to the migration transfer mods log.
 *
 * 'ns' name of the collection in which the operation will occur.
 * 'pattern' contains the _id value of the doc being updated.
 * 'notInActiveChunk' if true indicates that the update is coming from a donor shard
 * in a current chunk migration, and so does not need to be entered in this shard's
 * outgoing transfer log.
 */
void logUpdateOpForSharding(OperationContext* txn,
                            const char* ns,
                            const BSONObj& pattern,
                            bool notInActiveChunk);

/**
 * If sharding is enabled, pass the delete along to MigrationSourceManager::logDeleteOp
 * to determine whether the delete should be logged to the migration transfer mods log.
 *
 * 'ns' name of the collection in which the operation will occur.
 * 'obj' contains the _id value of the doc being deleted.
 * 'notInActiveChunk' a true value indicates that either:
 *      1) the delete is coming from a donor shard in a current chunk migration,
 *         and so does not need to be entered in this shard's outgoing transfer log.
 *      2) the document is not within this shard's outgoing chunk migration range,
 *         and so does not need to be forwarded to the migration recipient via the transfer log.
 */
void logDeleteOpForSharding(OperationContext* txn,
                            const char* ns,
                            const BSONObj& obj,
                            bool notInActiveChunk);

/**
 * Checks if 'doc' in 'ns' belongs to a currently migrating chunk.
 *
 * Note: Must be holding global IX lock when calling this method.
 */
bool isInMigratingChunk(OperationContext* txn, const NamespaceString& ns, const BSONObj& doc);
}
