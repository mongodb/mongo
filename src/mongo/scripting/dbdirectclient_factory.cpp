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

#include "mongo/scripting/dbdirectclient_factory.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

namespace {

const ServiceContext::Decoration<DBDirectClientFactory> forService =
    ServiceContext::declareDecoration<DBDirectClientFactory>();

}  // namespace

DBDirectClientFactory& DBDirectClientFactory::get(ServiceContext* context) {
    fassert(40151, context);
    return forService(context);
}

DBDirectClientFactory& DBDirectClientFactory::get(OperationContext* opCtx) {
    fassert(40152, opCtx);
    return get(opCtx->getServiceContext());
}

void DBDirectClientFactory::registerImplementation(Impl implementation) {
    _implementation = std::move(implementation);
}

auto DBDirectClientFactory::create(OperationContext* opCtx) -> Result {
    uassert(40153, "Cannot create a direct client in this context", _implementation);
    return _implementation(opCtx);
}

}  // namespace mongo
