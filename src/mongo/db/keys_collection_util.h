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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace keys_collection_util {

/*
 * Creates an ExternalKeysCollectionDocument representing an config.external_validation_keys
 * document created based on the given the admin.system.keys document BSONObj.
 */
ExternalKeysCollectionDocument makeExternalClusterTimeKeyDoc(BSONObj keyDoc,
                                                             boost::optional<Date_t> expireAt);

/*
 * Upserts the given ExternalKeysCollectionDocuments into the
 * config.external_validation_keys collection, and returns the optime for the upserts.
 */
repl::OpTime storeExternalClusterTimeKeyDocs(OperationContext* opCtx,
                                             std::vector<ExternalKeysCollectionDocument> keyDocs);

}  // namespace keys_collection_util
}  // namespace mongo
