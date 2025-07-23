/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/expression_context.h"

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

enum class ChangeStreamType {
    // Collection-level change stream: change stream is opened on a single collection.
    kCollection,

    // Database-level change stream: change stream is opened on all collections of a single
    // database.
    kDatabase,

    // Deployment-level change stream: change stream is opened on all databases / all collections
    // that have existed or will exist in
    // the system.
    kAllDatabases,
};

/**
 * Represents a change stream instance.
 */
class ChangeStream {
public:
    ChangeStream(ChangeStreamReadMode mode,
                 ChangeStreamType type,
                 boost::optional<NamespaceString> nss);

    /**
     * Returns the robustness level of the change stream instance.
     */
    ChangeStreamReadMode getReadMode() const;

    /**
     * Returns the type of the change stream.
     */
    ChangeStreamType getChangeStreamType() const;

    /**
     * Returns:
     * - The (fully-qualified) name of the collection, if the change stream is of type
     *   'ChangeStreamType::kCollection'.
     * - The name of the database, if the change stream is of type 'ChangeStreamType::kDatabase'.
     * - Nothing, if the change stream is of type 'ChangeStreamType::kAllDatabases'.
     */
    boost::optional<NamespaceString> getNamespace() const;

    /**
     * Extract the change stream type from the given nss.
     */
    static ChangeStreamType getChangeStreamType(const NamespaceString& nss);

    /**
     * Builds a 'ChangeStream' object from the parameters stored in the 'ExpressionContext'.
     */
    static ChangeStream buildFromExpressionContext(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    const ChangeStreamReadMode _mode;
    const ChangeStreamType _type;
    const boost::optional<NamespaceString> _nss;
};

}  // namespace mongo
