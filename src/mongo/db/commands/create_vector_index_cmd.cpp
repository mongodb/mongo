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

#include "mongo/db/create_vector_index_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace mongo {
namespace {

/**
 * { createVectorIndex : "bar",
 *   field: "baz",
 *   dimensions: 4 }
 */
class CmdCreateVectorIndex
    : public CreateVectorIndexCmdVersion1Gen<CmdCreateVectorIndex> {
public:
  bool allowedWithSecurityToken() const final { return true; }
  class Invocation final : public InvocationBase {
  public:
    using InvocationBase::InvocationBase;

    bool supportsWriteConcern() const final { return true; }

    NamespaceString ns() const final { return request().getNamespace(); }

    void doCheckAuthorization(OperationContext *opCtx) const {
      // TODO
    }

    CreateVectorIndexReply typedRun(OperationContext *opCtx) {
      CreateVectorIndexReply reply;
      // TODO
      return reply;
    }
  };

  bool collectsResourceConsumptionMetrics() const final { return true; }

  AllowedOnSecondary secondaryAllowed(ServiceContext *) const final {
    return AllowedOnSecondary::kNever;
  }

  bool allowedInTransactions() const final { return true; }

} cmdCreateVectorIndex;

} // namespace
} // namespace mongo
