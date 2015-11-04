/**
 *    Copyright 2015 MongoDB Inc.
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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/jsobj.h"

namespace mongo {
struct CollectionOptions;
class NamespaceString;
class OperationContext;

struct oplogUpdateEntryArgs {
    std::string ns;
    BSONObj update;
    BSONObj criteria;
    bool fromMigrate;
};

class OpObserver {
    MONGO_DISALLOW_COPYING(OpObserver);

public:
    OpObserver() {}
    ~OpObserver() {}
    void onCreateIndex(OperationContext* txn,
                       const std::string& ns,
                       BSONObj indexDoc,
                       bool fromMigrate = false);
    void onInserts(OperationContext* txn,
                   const NamespaceString& ns,
                   std::vector<BSONObj>::iterator begin,
                   std::vector<BSONObj>::iterator end,
                   bool fromMigrate = false);
    void onUpdate(OperationContext* txn, oplogUpdateEntryArgs args);
    void onDelete(OperationContext* txn,
                  const std::string& ns,
                  const BSONObj& idDoc,
                  bool fromMigrate = false);
    void onOpMessage(OperationContext* txn, const BSONObj& msgObj);
    void onCreateCollection(OperationContext* txn,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options);
    void onCollMod(OperationContext* txn, const std::string& dbName, const BSONObj& collModCmd);
    void onDropDatabase(OperationContext* txn, const std::string& dbName);
    void onDropCollection(OperationContext* txn, const NamespaceString& collectionName);
    void onDropIndex(OperationContext* txn,
                     const std::string& dbName,
                     const BSONObj& idxDescriptor);
    void onRenameCollection(OperationContext* txn,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            bool dropTarget,
                            bool stayTemp);
    void onApplyOps(OperationContext* txn, const std::string& dbName, const BSONObj& applyOpCmd);
    void onEmptyCapped(OperationContext* txn, const NamespaceString& collectionName);
    void onConvertToCapped(OperationContext* txn,
                           const NamespaceString& collectionName,
                           double size);
};

}  // namespace mongo
