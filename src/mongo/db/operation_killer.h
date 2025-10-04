/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/service_context.h"

namespace mongo {

/**
 * OperationKiller is a utility class to facilitate interrupting other operations
 *
 * This class is constructed with a pointer to the Client which is attempting to perform the
 * interruption. This Client must have a role with the killop action type or own the targeted
 * OperationContext. The Client must also be a member of the same ServiceContext as the targeted
 * OperationContext.
 */
class OperationKiller {
public:
    explicit OperationKiller(Client* myClient);

    /**
     * Verify that myClient has permission to kill operations it does not own
     */
    bool isGenerallyAuthorizedToKill() const;

    /**
     * Verify that the target exists and the myClient is allowed to kill it
     */
    bool isAuthorizedToKill(const ClientLock& target) const;

    /**
     * Kill an operation running on this instance of mongod or mongos.
     */
    void killOperation(OperationId opId);
    void killOperation(const OperationKey& opKey);

private:
    Client* const _myClient;
};

}  // namespace mongo
