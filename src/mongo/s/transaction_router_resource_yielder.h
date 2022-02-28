/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/db/resource_yielder.h"

namespace mongo {

/**
 * Implementation of ResourceYielder that yields resources checked out in the course of running a
 * distributed transaction.
 */
class TransactionRouterResourceYielder : public ResourceYielder {
public:
    /**
     * Returns a newly allocated yielder.
     */
    static std::unique_ptr<TransactionRouterResourceYielder> make();

    /**
     * If the opCtx has a checked out RouterOperationContextSession, yields it.
     */
    void yield(OperationContext* opCtx) override;

    /**
     * If the opCtx had previously checked out RouterOperationContextSession, checks it back out.
     * Note this may throw if the opCtx has been interrupted.
     */
    void unyield(OperationContext* opCtx) override;

private:
    bool _yielded{false};
};

}  // namespace mongo
