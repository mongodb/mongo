// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/views/util.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::view_util {
void validateViewDefinitionBSON(OperationContext* opCtx,
                                const BSONObj& viewDefinition,
                                const DatabaseName& dbName) {
    // Internal callers should always pass in a valid 'dbName' against which to compare the
    // 'viewDefinition'.
    invariant(DatabaseName::isValid(dbName));

    bool valid = true;

    for (auto&& [name, value] : viewDefinition) {
        valid &= name == "_id" || name == "viewOn" || name == "pipeline" || name == "collation" ||
            name == "timeseries";
    }

    auto viewNameElem = viewDefinition["_id"];
    valid &= viewNameElem && viewNameElem.type() == BSONType::string;

    auto viewName = NamespaceStringUtil::deserialize(
        dbName.tenantId(), viewNameElem.str(), SerializationContext::stateDefault());

    bool viewNameIsValid = NamespaceString::validCollectionComponent(viewName) &&
        DatabaseName::isValid(viewName.dbName());
    valid &= viewNameIsValid;

    // Only perform validation via NamespaceString if the collection name has been determined to
    // be valid. If not valid then the NamespaceString constructor will uassert.
    if (viewNameIsValid) {
        valid &= viewName.isValid() && viewName.dbName() == dbName;
    }

    valid &= NamespaceString::validCollectionName(viewNameElem.str());

    auto viewOn = viewDefinition["viewOn"];
    valid &= viewOn && viewOn.type() == BSONType::string;

    if (auto pipeline = viewDefinition["pipeline"]) {
        valid &= pipeline.type() == BSONType::array;
        for (auto&& stage : pipeline.Obj()) {
            valid &= stage.type() == BSONType::object;
        }
    }

    auto collation = viewDefinition["collation"];
    valid &= !collation || collation.type() == BSONType::object;

    auto timeseries = viewDefinition["timeseries"];
    valid &= !timeseries || timeseries.type() == BSONType::object;

    uassert(
        ErrorCodes::InvalidViewDefinition,
        str::stream() << "found invalid view definition " << viewDefinition["_id"]
                      << " while reading '"
                      << NamespaceString::makeSystemDotViewsNamespace(dbName).toStringForErrorMsg()
                      << "'",
        valid);
}

ParsedViewDefinition parseViewDefinitionBSON(OperationContext* opCtx,
                                             const DatabaseName& dbName,
                                             const BSONObj& view) {
    try {
        view_util::validateViewDefinitionBSON(opCtx, view, dbName);
    } catch (const DBException& ex) {
        return {.viewName = boost::none, .viewDefinition = std::move(ex.toStatus())};
    }

    auto viewName = NamespaceStringUtil::deserialize(
        dbName.tenantId(), view.getStringField("_id"), SerializationContext::stateDefault());
    auto collatorElem = view["collation"];
    auto collator = collatorElem && !collatorElem.Obj().isEmpty()
        ? CollatorFactoryInterface::get(opCtx->getServiceContext())
              ->makeFromBSON(collatorElem.Obj())
        : nullptr;
    if (!collator.isOK()) {
        return {.viewName = std::move(viewName), .viewDefinition = std::move(collator.getStatus())};
    }

    auto viewDefinition =
        std::make_shared<ViewDefinition>(viewName.dbName(),
                                         viewName.coll(),
                                         view.getStringField("viewOn"),
                                         BSONArray{view.getObjectField("pipeline")},
                                         std::move(collator.getValue()));
    return {.viewName = std::move(viewName), .viewDefinition = std::move(viewDefinition)};
}

}  // namespace mongo::view_util
