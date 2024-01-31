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

#pragma once

#include "mongo/db/operation_context.h"

namespace mongo {
namespace repl {

/**
 * Writes oplog entries to the oplog and/or the change collection.
 * TODO (SERVER-85431): make this a runnable component.
 */
class OplogWriter {
    OplogWriter(const OplogWriter&) = delete;
    OplogWriter& operator=(const OplogWriter&) = delete;

public:
    /**
     * Used to configure the behavior of this OplogWriter.
     */
    class Options {
    public:
        Options() : skipWritesToOplogColl(false) {}
        explicit Options(bool skipWritesToOplogColl)
            : skipWritesToOplogColl(skipWritesToOplogColl) {}

        const bool skipWritesToOplogColl;
    };

    /**
     * Constructs this OplogWriter with specific options.
     */
    OplogWriter(const Options& options);

    virtual ~OplogWriter() = default;

    /**
     * Writes a batch of oplog entries to the oplog and/or the change collection.
     *
     * If the batch write is successful, returns the optime of the last op written,
     * which should be the last op in the batch.
     *
     * Oplog visibility and updates to replication coordinator timestamps should be
     * handled by caller.
     */
    virtual StatusWith<OpTime> writeOplogBatch(OperationContext* opCtx,
                                               std::vector<BSONObj> ops) = 0;

    const Options& getOptions() const;

private:
    // Configures this OplogWriter.
    const Options _options;
};

}  // namespace repl
}  // namespace mongo
