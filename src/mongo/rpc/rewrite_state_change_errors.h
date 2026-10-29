// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/message.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::rpc {

class [[MONGO_MOD_PUBLIC]] RewriteStateChangeErrors {
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
     *   - `doc` contains error nodes that need rewriting.
     *       - Codes in the `NotPrimaryError` category always need rewriting.
     *       - Codes in the `ShutdownError` category need rewriting unless this
     *         server is shutting down.
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
