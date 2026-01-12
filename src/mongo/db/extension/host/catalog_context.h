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

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>

namespace mongo::extension::host {
/**
 * CatalogContext is a helper class which is used to provide the boundary type
 * ::MongoExtensionCatalogContext to extensions. The lifetime of the boundary type's views is bound
 * to this helper class.
 */
class CatalogContext {
public:
    CatalogContext(const ExpressionContext& expCtx)
        : _dbName(DatabaseNameUtil::serialize(expCtx.getNamespaceString().dbName(),
                                              SerializationContext::stateCommandRequest())),
          _collName(expCtx.getNamespaceString().coll()),
          _uuid(expCtx.getUUID().has_value() ? expCtx.getUUID()->toString() : ""),
          _api(::MongoExtensionNamespaceString(stringDataAsByteView(StringData(_dbName)),
                                               stringDataAsByteView(StringData(_collName))),
               stringDataAsByteView(StringData(_uuid))) {}

    ~CatalogContext() = default;

    const ::MongoExtensionCatalogContext& getAsBoundaryType() const {
        return _api;
    }

private:
    const std::string _dbName;
    const std::string _collName;
    const std::string _uuid;
    const ::MongoExtensionCatalogContext _api;
};
};  // namespace mongo::extension::host
