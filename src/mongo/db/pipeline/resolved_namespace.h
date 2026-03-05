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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Represents an underlying namespace used for *secondary* namespaces in the query
 * (e.g. the `from` collection in a $lookup or $unionWith):
 *  - For a regular collection, 'ns' is the collection itself and 'pipeline' is empty.
 *  - For a view, 'ns' is the underlying source collection and 'pipeline' is the
 *    view definition pipeline that must be prepended to any sub-pipeline targeting it.
 *
 * The primary (or top-level) namespace's view information is stored separately via ViewInfo
 * on the ExpressionContext.
 */
struct MONGO_MOD_PUBLIC ResolvedNamespace {
    ResolvedNamespace() = default;
    ResolvedNamespace(NamespaceString ns,
                      std::vector<BSONObj> pipeline,
                      boost::optional<UUID> collUUID = boost::none,
                      bool involvedNamespaceIsAView = false)
        : ns(std::move(ns)),
          pipeline(std::move(pipeline)),
          uuid(collUUID),
          involvedNamespaceIsAView(involvedNamespaceIsAView) {}

    NamespaceString ns;
    std::vector<BSONObj> pipeline;
    boost::optional<UUID> uuid = boost::none;
    bool involvedNamespaceIsAView = false;
    // TODO (SERVER-100170): Add a LiteParsedPipeline member. We often need this information when
    // resolving views and currently recompute the object every time it's requested. Once added, go
    // through the rest of the codebase to ensure that we aren't unnecessarily creating a
    // LiteParsedPipeline object when it's already being stored here.
};

/**
 * Map from view to resolved namespace.
 */
using ResolvedNamespaceMap MONGO_MOD_PUBLIC =
    absl::flat_hash_map<NamespaceString, ResolvedNamespace>;

}  // namespace mongo
