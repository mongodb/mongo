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

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

namespace mongo {
class ShardingDDLCoordinatorExternalState {
public:
    virtual ~ShardingDDLCoordinatorExternalState() = default;
    virtual void checkShardedDDLAllowedToStart(OperationContext* opCtx,
                                               const NamespaceString& nss) const = 0;
    virtual void waitForVectorClockDurable(OperationContext* opCtx) const = 0;
    virtual void assertIsPrimaryShardForDb(OperationContext* opCtx,
                                           const DatabaseName& dbName) const = 0;
    virtual bool isTrackedTimeseries(OperationContext* opCtx,
                                     const NamespaceString& bucketNss) const = 0;
    virtual void allowMigrations(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 bool allowMigrations) = 0;
    virtual bool checkAllowMigrations(OperationContext* opCtx, const NamespaceString& nss) = 0;

private:
};

class ShardingDDLCoordinatorExternalStateImpl : public ShardingDDLCoordinatorExternalState {
public:
    void checkShardedDDLAllowedToStart(OperationContext* opCtx,
                                       const NamespaceString& nss) const override;
    void waitForVectorClockDurable(OperationContext* opCtx) const override;
    void assertIsPrimaryShardForDb(OperationContext* opCtx,
                                   const DatabaseName& dbName) const override;
    bool isTrackedTimeseries(OperationContext* opCtx,
                             const NamespaceString& bucketNss) const override;
    void allowMigrations(OperationContext* opCtx,
                         const NamespaceString& nss,
                         bool allowMigrations) override;
    bool checkAllowMigrations(OperationContext* opCtx, const NamespaceString& nss) override;
};

class ShardingDDLCoordinatorExternalStateFactory {
public:
    virtual ~ShardingDDLCoordinatorExternalStateFactory() = default;
    virtual std::shared_ptr<ShardingDDLCoordinatorExternalState> create() const = 0;
};

class ShardingDDLCoordinatorExternalStateFactoryImpl
    : public ShardingDDLCoordinatorExternalStateFactory {
public:
    std::shared_ptr<ShardingDDLCoordinatorExternalState> create() const override;
};


}  // namespace mongo
