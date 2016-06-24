/**
*    Copyright (C) 2016 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/views/view_catalog.h"

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/views/view.h"

namespace {
bool enableViews = false;
}  // namespace

namespace mongo {
ExportedServerParameter<bool, ServerParameterType::kStartupOnly> enableViewsParameter(
    ServerParameterSet::getGlobal(), "enableViews", &enableViews);

const std::uint32_t ViewCatalog::kMaxViewDepth = 20;

ViewCatalog::ViewCatalog(OperationContext* txn, Database* database) {}

Status ViewCatalog::createView(OperationContext* txn,
                               const NamespaceString& viewName,
                               const NamespaceString& viewOn,
                               const BSONObj& pipeline) {
    if (!enableViews)
        return Status(ErrorCodes::CommandNotSupported, "View support not enabled");

    if (lookup(StringData(viewName.ns())))
        return Status(ErrorCodes::NamespaceExists, "Namespace already exists");

    if (viewName.db() != viewOn.db())
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    BSONObj ownedPipeline = pipeline.getOwned();
    txn->recoveryUnit()->onCommit([this, viewName, viewOn, ownedPipeline]() {
        _viewMap[viewName.ns()] = std::make_shared<ViewDefinition>(
            viewName.db(), viewName.coll(), viewOn.coll(), ownedPipeline);
    });
    return Status::OK();
}

ViewDefinition* ViewCatalog::lookup(StringData ns) {
    ViewMap::const_iterator it = _viewMap.find(ns);
    if (it != _viewMap.end()) {
        return it->second.get();
    }
    return nullptr;
}
}  // namespace mongo
