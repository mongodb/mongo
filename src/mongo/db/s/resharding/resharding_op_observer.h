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

#include <boost/optional.hpp>

#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/storage/durable_history_pin.h"

namespace mongo {

class ReshardingHistoryHook : public DurableHistoryPin {
public:
    static constexpr StringData kName = "resharding"_sd;

    std::string getName() override {
        return kName.toString();
    }

    boost::optional<Timestamp> calculatePin(OperationContext* opCtx) override;
};

/**
 * OpObserver for observing writes to internal resharding collections. This includes collections
 * such as config.reshardingOperations, config.localReshardingOperations.donor, and
 * config.localReshardingOperations.recipient.
 */
class ReshardingOpObserver final : public OpObserverNoop {
    ReshardingOpObserver(const ReshardingOpObserver&) = delete;
    ReshardingOpObserver& operator=(const ReshardingOpObserver&) = delete;

public:
    ReshardingOpObserver();
    ~ReshardingOpObserver() override;

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   InsertsOpStateAccumulator* opAccumulator = nullptr) override;

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override;

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override;
};

}  // namespace mongo
