// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <iosfwd>
#include <string>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

enum class [[MONGO_MOD_PUBLIC]] ChangeStreamType {
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
class [[MONGO_MOD_PUBLIC]] ChangeStream {
public:
    ChangeStream(ChangeStreamReadMode mode,
                 ChangeStreamType type,
                 const boost::optional<NamespaceString>& nss);

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
     * Return a string representation of the ChangeStream object.
     */
    std::string toString() const;

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

std::ostream& operator<<(std::ostream& os, const ChangeStream& changeStream);

}  // namespace mongo
