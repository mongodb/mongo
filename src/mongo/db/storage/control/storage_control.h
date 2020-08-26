/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

namespace mongo {

class OperationContext;
class ServiceContext;
class Status;

/**
 * Helper functions to manipulate independent processes that perform actions against the storage
 * engine.
 */
namespace StorageControl {

/**
 * Responsible for initializing independent processes for replication that interact with the storage
 * layer.
 *
 * Instantiates the JournalFlusher to flush writes to disk periodically and upon request. If
 * 'forTestOnly' is set, then the JournalFlusher will only run upon request so as not to disrupt
 * unit test expectations.
 *
 * Safe to call again after stopStorageControls() has been called, to restart any processes that
 * were stopped.
 */
void startStorageControls(ServiceContext* serviceContext, bool forTestOnly = false);

/**
 * Stops the processes begun by startStorageControls() and relays the reason to them.
 *
 * The JournalFlusher is shut down.
 *
 * Safe to call multiple times, whether or not startStorageControls() has been called.
 */
void stopStorageControls(ServiceContext* serviceContext, const Status& reason);

/**
 * Prompts an immediate journal flush and returns without waiting for it.
 */
void triggerJournalFlush(ServiceContext* serviceContext);

/**
 * Initiates if needed and waits for a complete round of journal flushing to execute.
 *
 * Can throw ShutdownInProgress if the storage engine is being closed.
 */
void waitForJournalFlush(OperationContext* opCtx);

/**
 * Ensures interruption of the JournalFlusher if it is or will be acquiring a lock.
 */
void interruptJournalFlusherForReplStateChange(ServiceContext* serviceContext);

}  // namespace StorageControl

}  // namespace mongo
