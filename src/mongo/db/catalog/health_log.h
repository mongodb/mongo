/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/concurrency/deferred_writer.h"
#include "mongo/db/service_context.h"

namespace mongo {

class HealthLogEntry;

/**
 * The interface to the local healthlog.
 *
 * This class contains facilities for creating and asynchronously writing to the local healthlog
 * collection.  There should only be one instance of this class, initialized on startup and cleaned
 * up on shutdown.
 */
class HealthLog {
    MONGO_DISALLOW_COPYING(HealthLog);

public:
    /**
     * Required to use HealthLog as a ServiceContext decorator.
     *
     * Should not be used anywhere else.
     */
    HealthLog();

    /**
     * The maximum size of the in-memory buffer of health-log entries, in bytes.
     */
    static const int64_t kMaxBufferSize = 25'000'000;

    /**
     * Start the worker thread writing the buffer to the collection.
     */
    void startup(void);

    /**
     * Stop the worker thread.
     */
    void shutdown(void);

    /**
     * The name of the collection.
     */
    static const NamespaceString nss;

    /**
     * Get the current context's HealthLog.
     */
    static HealthLog& get(ServiceContext* ctx);
    static HealthLog& get(OperationContext* ctx);

    /**
     * Asynchronously insert the given entry.
     *
     * Return `false` iff there is no more space in the buffer.
     */
    bool log(const HealthLogEntry& entry);

private:
    DeferredWriter _writer;
};
}
