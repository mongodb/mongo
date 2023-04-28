/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/op_observer/op_observer_noop.h"

namespace mongo {

/**
 * OpObserver for time-series collections. Notify the Bucket Catalog of events so it can update its
 * state.
 */
class TimeSeriesOpObserver final : public OpObserverNoop {
    TimeSeriesOpObserver(const TimeSeriesOpObserver&) = delete;
    TimeSeriesOpObserver& operator=(const TimeSeriesOpObserver&) = delete;

public:
    TimeSeriesOpObserver() = default;
    ~TimeSeriesOpObserver() = default;

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator first,
                   std::vector<InsertStatement>::const_iterator last,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   InsertsOpStateAccumulator* opAccumulator = nullptr) final;

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;

    void aboutToDelete(OperationContext* opCtx,
                       const CollectionPtr& coll,
                       const BSONObj& doc) final;

    void onDropDatabase(OperationContext* opCtx, const DatabaseName& dbName) final;

    using OpObserver::onDropCollection;
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  CollectionDropType dropType) final;

private:
    void _onReplicationRollback(OperationContext* opCtx, const RollbackObserverInfo& rbInfo);
};

}  // namespace mongo
