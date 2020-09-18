/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/repl/tenant_migration_donor_op_observer.h"
#include "mongo/db/repl/tenant_migration_donor_util.h"

namespace mongo {
namespace repl {

namespace {

const auto databasePrefixToDeleteDecoration = OperationContext::declareDecoration<std::string>();

/**
 * Used to remove the in-memory state for the migration denoted by the donor's state doc once the
 * write for deleting the doc is committed.
 */
class TenantMigrationDonorDeletionHandler final : public RecoveryUnit::Change {
public:
    TenantMigrationDonorDeletionHandler(OperationContext* opCtx, const std::string dbPrefix)
        : _opCtx(opCtx), _dbPrefix(dbPrefix) {}

    void commit(boost::optional<Timestamp>) override {
        tenant_migration_donor::onDelete(_opCtx, _dbPrefix);
    }

    void rollback() override {}

private:
    OperationContext* _opCtx;
    const std::string _dbPrefix;
};

}  // namespace

void TenantMigrationDonorOpObserver::onInserts(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               OptionalCollectionUUID uuid,
                                               std::vector<InsertStatement>::const_iterator first,
                                               std::vector<InsertStatement>::const_iterator last,
                                               bool fromMigrate) {
    if (nss == NamespaceString::kTenantMigrationDonorsNamespace) {
        for (auto it = first; it != last; it++) {
            tenant_migration_donor::onInsertOrUpdate(opCtx, it->doc);
        }
    }
}

void TenantMigrationDonorOpObserver::onUpdate(OperationContext* opCtx,
                                              const OplogUpdateEntryArgs& args) {
    if (args.nss == NamespaceString::kTenantMigrationDonorsNamespace) {
        tenant_migration_donor::onInsertOrUpdate(opCtx, args.updateArgs.updatedDoc);
    }
}

void TenantMigrationDonorOpObserver::aboutToDelete(OperationContext* opCtx,
                                                   NamespaceString const& nss,
                                                   BSONObj const& doc) {
    if (nss == NamespaceString::kTenantMigrationDonorsNamespace) {
        auto donorStateDoc =
            TenantMigrationDonorDocument::parse(IDLParserErrorContext("donorStateDoc"), doc);
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "cannot delete a donor's state document " << doc
                              << " since it has not been marked as garbage collectable",
                donorStateDoc.getExpireAt());
        databasePrefixToDeleteDecoration(opCtx) = donorStateDoc.getDatabasePrefix().toString();
    }
}

void TenantMigrationDonorOpObserver::onDelete(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              OptionalCollectionUUID uuid,
                                              StmtId stmtId,
                                              bool fromMigrate,
                                              const boost::optional<BSONObj>& deletedDoc) {
    if (nss == NamespaceString::kTenantMigrationDonorsNamespace) {
        opCtx->recoveryUnit()->registerChange(std::make_unique<TenantMigrationDonorDeletionHandler>(
            opCtx, databasePrefixToDeleteDecoration(opCtx)));
    }
}

}  // namespace repl
}  // namespace mongo
