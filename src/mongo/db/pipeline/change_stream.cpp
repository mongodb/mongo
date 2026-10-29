// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/util/assert_util.h"

#include <iostream>
#include <string_view>

#include <fmt/format.h>

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {
boost::optional<NamespaceString> convertedNss(ChangeStreamType type,
                                              const boost::optional<NamespaceString>& nss) {
    if (type == ChangeStreamType::kAllDatabases) {
        // Cluster-wide change streams have to be opened on the "admin" database.
        // Convert this to a Namespace without a value.
        tassert(10656200,
                "NSS for all-databases change stream must be empty",
                !nss.has_value() || nss->isAdminDB());
        return {};
    }

    tassert(10656201,
            "NSS for collection- or database-level change stream must not be empty",
            nss.has_value());

    if (type == ChangeStreamType::kDatabase && nss->isCollectionlessAggregateNS()) {
        // Convert 'dbName.$cmd.aggregate' namespace to just 'dbName'.
        return NamespaceString(nss->dbName());
    }

    tassert(10656202,
            "NSS for collection- or database-level change stream must have the right granularity",
            (type == ChangeStreamType::kDatabase) == nss->isDbOnly());

    // Return original namespace as is.
    return nss;
}

}  // namespace
ChangeStream::ChangeStream(ChangeStreamReadMode mode,
                           ChangeStreamType type,
                           const boost::optional<NamespaceString>& nss)
    : _mode(mode), _type(type), _nss(convertedNss(type, nss)) {}

ChangeStreamReadMode ChangeStream::getReadMode() const {
    return _mode;
}

ChangeStreamType ChangeStream::getChangeStreamType() const {
    return _type;
}

boost::optional<NamespaceString> ChangeStream::getNamespace() const {
    return _nss;
}

std::string ChangeStream::toString() const {
    std::string_view mode =
        _mode == ChangeStreamReadMode::kStrict ? "strict" : "ignoreRemovedShards";
    std::string_view type = [&]() -> std::string_view {
        switch (_type) {
            case ChangeStreamType::kAllDatabases:
                return "all-databases"sv;
            case ChangeStreamType::kDatabase:
                return "database"sv;
            case ChangeStreamType::kCollection:
                return "collection"sv;
        }
        MONGO_UNREACHABLE_TASSERT(10657559);
    }();

    if (_type == ChangeStreamType::kAllDatabases) {
        return fmt::format("ChangeStream (type: {}, mode: {})", type, mode);
    } else {
        return fmt::format("ChangeStream (type: {}, mode: {}, nss: '{}')",
                           type,
                           mode,
                           NamespaceStringUtil::serialize(*_nss, SerializationContext{}));
    }
}

ChangeStreamType ChangeStream::getChangeStreamType(const NamespaceString& nss) {
    // If we have been permitted to run on admin, 'allChangesForCluster' must be true.
    return (nss.isAdminDB() ? ChangeStreamType::kAllDatabases
                            : (nss.isCollectionlessAggregateNS() ? ChangeStreamType::kDatabase
                                                                 : ChangeStreamType::kCollection));
}

ChangeStream ChangeStream::buildFromExpressionContext(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    tassert(10743905,
            "expecting changeStreamSpec to be present in ExpressionContext",
            expCtx->getChangeStreamSpec().has_value());

    const auto& nss = expCtx->getNamespaceString();

    return ChangeStream(fromIgnoreRemovedShardsParameter(static_cast<bool>(
                            expCtx->getChangeStreamSpec()->getIgnoreRemovedShards())),
                        getChangeStreamType(nss),
                        nss);
}

std::ostream& operator<<(std::ostream& os, const ChangeStream& changeStream) {
    os << changeStream.toString();
    return os;
};

}  // namespace mongo
