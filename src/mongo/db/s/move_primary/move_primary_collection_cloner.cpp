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

#include "mongo/db/s/move_primary/move_primary_collection_cloner.h"

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/database_cloner_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kMovePrimary

namespace mongo {

MovePrimaryCollectionCloner::MovePrimaryCollectionCloner(const CollectionParams& collectionParams,
                                                         MovePrimarySharedData* sharedData,
                                                         const HostAndPort& source,
                                                         DBClientConnection* client,
                                                         repl::StorageInterface* storageInterface,
                                                         ThreadPool* dbPool)
    : MovePrimaryBaseCloner(
          "MovePrimaryCollectionCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _collectionParams(collectionParams),
      _countStage("count", this, &MovePrimaryCollectionCloner::countStage),
      _checkIfDonorCollectionIsEmptyStage(
          "checkIfDonorCollectionIsEmpty",
          this,
          &MovePrimaryCollectionCloner::checkIfDonorCollectionIsEmptyStage),
      _listIndexesStage("listIndexes", this, &MovePrimaryCollectionCloner::listIndexesStage),
      _createCollectionStage(
          "createCollection", this, &MovePrimaryCollectionCloner::createCollectionStage),
      _queryStage("query", this, &MovePrimaryCollectionCloner::queryStage) {}

repl::BaseCloner::ClonerStages MovePrimaryCollectionCloner::getStages() {
    return {&_countStage,
            &_checkIfDonorCollectionIsEmptyStage,
            &_listIndexesStage,
            &_createCollectionStage,
            &_queryStage};
}

void MovePrimaryCollectionCloner::preStage() {}

void MovePrimaryCollectionCloner::postStage() {}

repl::BaseCloner::AfterStageBehavior
MovePrimaryCollectionCloner::MovePrimaryCollectionClonerStage::run() {
    return ClonerStage<MovePrimaryCollectionCloner>::run();
}

repl::BaseCloner::AfterStageBehavior MovePrimaryCollectionCloner::countStage() {
    return kContinueNormally;
}

repl::BaseCloner::AfterStageBehavior
MovePrimaryCollectionCloner::checkIfDonorCollectionIsEmptyStage() {
    return kContinueNormally;
}

repl::BaseCloner::AfterStageBehavior MovePrimaryCollectionCloner::listIndexesStage() {
    return kContinueNormally;
}

repl::BaseCloner::AfterStageBehavior MovePrimaryCollectionCloner::createCollectionStage() {
    return kContinueNormally;
}

repl::BaseCloner::AfterStageBehavior MovePrimaryCollectionCloner::queryStage() {
    return kContinueNormally;
}
}  // namespace mongo
