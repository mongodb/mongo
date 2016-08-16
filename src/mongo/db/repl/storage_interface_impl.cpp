/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/storage_interface_impl.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context_impl.h"

namespace mongo {
namespace repl {

StorageInterfaceImpl::StorageInterfaceImpl() : StorageInterface() {}
StorageInterfaceImpl::~StorageInterfaceImpl() {}

OperationContext* StorageInterfaceImpl::createOperationContext() {
    if (!ClientBasic::getCurrent()) {
        Client::initThreadIfNotAlready();
        AuthorizationSession::get(*ClientBasic::getCurrent())->grantInternalAuthorization();
    }
    return new OperationContextImpl();
}

StatusWith<size_t> StorageInterfaceImpl::getOplogMaxSize(OperationContext* txn,
                                                         const NamespaceString& nss) {
    AutoGetCollectionForRead collection(txn, nss);
    if (!collection.getCollection()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Your oplog doesn't exist: " << nss.ns()};
    }

    const auto options = collection.getCollection()->getCatalogEntry()->getCollectionOptions(txn);
    if (!options.capped)
        return {ErrorCodes::BadValue, str::stream() << nss.ns() << " isn't capped"};

    return options.cappedSize;
}

}  // namespace repl
}  // namespace mongo
