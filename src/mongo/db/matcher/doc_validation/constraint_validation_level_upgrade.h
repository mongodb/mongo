/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Reads the collection validator from the local catalog and runs an aggregation to find any
 * document that violates it. Returns a non-OK status if such a document exists, or Status::OK()
 * if all documents conform or if the collection has no validator.
 *
 * 'placementConcern' is used only when acquiring the collection to read its validator; the
 * subsequent aggregation acquires the collection itself or sends commands to remote shards.
 */
MONGO_MOD_PUBLIC Status noDocumentsViolatingValidator(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      PlacementConcern placementConcern);

}  // namespace mongo
