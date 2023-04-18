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

#include "mongo/db/views/util.h"

#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_feature_flags_gen.h"

namespace mongo::view_util {
void validateViewDefinitionBSON(OperationContext* opCtx,
                                const BSONObj& viewDefinition,
                                const DatabaseName& dbName) {
    // Internal callers should always pass in a valid 'dbName' against which to compare the
    // 'viewDefinition'.
    invariant(NamespaceString::validDBName(dbName));

    bool valid = true;

    for (auto&& [name, value] : viewDefinition) {
        valid &= name == "_id" || name == "viewOn" || name == "pipeline" || name == "collation" ||
            name == "timeseries";
    }

    auto viewNameElem = viewDefinition["_id"];
    valid &= viewNameElem && viewNameElem.type() == BSONType::String;

    auto viewName = NamespaceStringUtil::deserialize(dbName.tenantId(), viewNameElem.str());

    bool viewNameIsValid = NamespaceString::validCollectionComponent(viewName.ns()) &&
        NamespaceString::validDBName(viewName.dbName());
    valid &= viewNameIsValid;

    // Only perform validation via NamespaceString if the collection name has been determined to
    // be valid. If not valid then the NamespaceString constructor will uassert.
    if (viewNameIsValid) {
        valid &= viewName.isValid() && viewName.dbName() == dbName;
    }

    valid &= NamespaceString::validCollectionName(viewNameElem.str());

    auto viewOn = viewDefinition["viewOn"];
    valid &= viewOn && viewOn.type() == BSONType::String;

    if (auto pipeline = viewDefinition["pipeline"]) {
        valid &= pipeline.type() == BSONType::Array;
        for (auto&& stage : pipeline.Obj()) {
            valid &= stage.type() == BSONType::Object;
        }
    }

    auto collation = viewDefinition["collation"];
    valid &= !collation || collation.type() == BSONType::Object;

    auto timeseries = viewDefinition["timeseries"];
    valid &= !timeseries || timeseries.type() == BSONType::Object;

    uassert(
        ErrorCodes::InvalidViewDefinition,
        str::stream() << "found invalid view definition " << viewDefinition["_id"]
                      << " while reading '"
                      << NamespaceString::makeSystemDotViewsNamespace(dbName).toStringForErrorMsg()
                      << "'",
        valid);
}
}  // namespace mongo::view_util
