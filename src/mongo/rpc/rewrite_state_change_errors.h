/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/message.h"

namespace mongo::rpc {

class RewriteStateChangeErrors {
public:
    /**
     * Enable/disable for an entire server.
     * The default is determined by server parameter "rewriteStateChangeErrors".
     */
    static bool getEnabled(ServiceContext* sc);
    static void setEnabled(ServiceContext* sc, bool e);

    /** Enable/disable only for a single operation. */
    static bool getEnabled(OperationContext* opCtx);
    static void setEnabled(OperationContext* opCtx, bool e);

    /**
     * State change codes are rewritten on the way out of a `mongos` server.
     * Errors injected via `failCommand` manipulation are normally exempt from
     * this. However, we provide an override option so they can be made subject
     * to rewriting if that's really necessary.
     */
    static void onActiveFailCommand(OperationContext* opCtx, const BSONObj& data);

    /**
     * Transforms an outgoing message to conditionally mask "state change" errors,
     * which are errors that cause a client to change its connection to the host
     * that sent it, such as marking it "Unknown". A shutdown error that's emitted
     * by a proxy server (e.g. mongos) can be misinterpreted by a naive client to
     * be indicative of the proxy server shutting down. So this simple rewrite
     * scheme attempts to mask those errors on the way out.
     *
     * Return a disengaged optional if no rewrite is needed.
     * Otherwise, returns a copy of doc with the errors rewritten.
     *
     * Error-bearing subobjects are places in the document where a `code` element
     * can indicate an error status. When rewriting, any error-bearing subobjects
     * are examined for a `code` element.
     *
     * In an error document (with "ok" mapped to 0.0), only the root element is
     * error-bearing. In a success document (where "ok" is mapped to 1.0), the
     * `writeConcernError` subobject and all the subobjects in the `writeErrors`
     * BSON array are error-bearing subobjects.
     *
     * Rewriting occurs only if all of these conditions are met:
     *
     *   - This feature wasn't deselected with a `setEnabled` call, either on the
     *     `opCtx` or for more widely for its associated ServiceContext.
     *     This will happen with failpoint-induced errors or with "hello" commands,
     *     both of which are exempt from state change error rewriting.
     *
     *   - doc contains no error nodes that need rewriting.
     *       - NotPrimaryError codes always need rewriting.
     *       - ShutdownError codes need rewriting unless this server is
     *         shutting down.
     *
     * Any rewritten `code` is replaced by `HostUnreachable`, and associated
     * `codeName` replaced to be consistent with the new code. Additionally, the
     * `errmsg` in the subobject has all occurrences of the key phrases "not
     * master" and "node is recovering" replaced with "(NOT_PRIMARY)" and
     * "(NODE_IS_RECOVERING)" respectively so that client state changes based on
     * the presence of these legacy strings are suppressed.
     */
    static boost::optional<BSONObj> rewrite(BSONObj doc, OperationContext* opCtx);
};

}  // namespace mongo::rpc
