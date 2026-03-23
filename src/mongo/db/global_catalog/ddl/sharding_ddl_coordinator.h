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

#include "mongo/db/global_catalog/ddl/sharding_coordinator.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

template <typename StateDoc>
class MONGO_MOD_NEEDS_REPLACEMENT NonRecoverableShardingDDLCoordinator
    : public ShardingCoordinator,
      protected NonRecoverableTypedDocMixin<StateDoc> {
protected:
    explicit NonRecoverableShardingDDLCoordinator(ShardingCoordinatorService* service,
                                                  std::string name,
                                                  const BSONObj& coorDoc)
        : ShardingCoordinator(service, std::move(name), coorDoc),
          NonRecoverableTypedDocMixin<StateDoc>(coorDoc) {}

    const CoordinatorStateDoc& getDoc() const override {
        return this->_docWrapper;
    }

    CoordinatorStateDoc& getDoc() override {
        return this->_docWrapper;
    }
};

template <typename StateDoc>
class MONGO_MOD_UNFORTUNATELY_OPEN RecoverableShardingDDLCoordinator
    : public RecoverableShardingCoordinator,
      protected RecoverableTypedDocMixin<RecoverableShardingDDLCoordinator<StateDoc>, StateDoc> {

    friend RecoverableTypedDocMixin<RecoverableShardingDDLCoordinator<StateDoc>, StateDoc>;

protected:
    explicit RecoverableShardingDDLCoordinator(ShardingCoordinatorService* service,
                                               std::string name,
                                               const BSONObj& coorDoc)
        : RecoverableShardingCoordinator(service, std::move(name), coorDoc),
          RecoverableTypedDocMixin<RecoverableShardingDDLCoordinator<StateDoc>, StateDoc>(coorDoc) {
    }

    const CoordinatorStateDoc& getDoc() const override {
        return this->_docWrapper;
    }

    CoordinatorStateDoc& getDoc() override {
        return this->_docWrapper;
    }

private:
    StringData serializeGenericPhase(CoordinatorGenericPhase phase) const override {
        return this->serializePhase(phase);
    }
};

#undef MONGO_LOGV2_DEFAULT_COMPONENT

}  // namespace mongo
