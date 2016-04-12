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


#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"

namespace mongo {
namespace repl {

class StorageInterfaceImpl : public StorageInterface {
    MONGO_DISALLOW_COPYING(StorageInterfaceImpl);

public:
    static const char kDefaultMinValidNamespace[];
    static const char kInitialSyncFlagFieldName[];
    static const char kBeginFieldName[];

    StorageInterfaceImpl();
    explicit StorageInterfaceImpl(const NamespaceString& minValidNss);

    /**
     * Returns namespace of collection containing the minvalid boundaries and initial sync flag.
     */
    NamespaceString getMinValidNss() const;

    ServiceContext::UniqueOperationContext createOperationContext(Client* client) override;

    bool getInitialSyncFlag(OperationContext* txn) const override;

    void setInitialSyncFlag(OperationContext* txn) override;

    void clearInitialSyncFlag(OperationContext* txn) override;

    BatchBoundaries getMinValid(OperationContext* txn) const override;

    void setMinValid(OperationContext* ctx,
                     const OpTime& endOpTime,
                     const DurableRequirement durReq) override;

    void setMinValid(OperationContext* ctx, const BatchBoundaries& boundaries) override;

private:
    NamespaceString _minValidNss;
};

}  // namespace repl
}  // namespace mongo
