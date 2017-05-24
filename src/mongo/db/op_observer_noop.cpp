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

#include "mongo/db/op_observer_noop.h"

namespace mongo {

void OpObserverNoop::onCreateIndex(
    OperationContext*, const NamespaceString&, OptionalCollectionUUID, BSONObj, bool) {}

void OpObserverNoop::onInserts(OperationContext*,
                               const NamespaceString&,
                               OptionalCollectionUUID,
                               std::vector<BSONObj>::const_iterator,
                               std::vector<BSONObj>::const_iterator,
                               bool) {}

void OpObserverNoop::onUpdate(OperationContext*, const OplogUpdateEntryArgs&) {}

CollectionShardingState::DeleteState OpObserverNoop::aboutToDelete(OperationContext*,
                                                                   const NamespaceString&,
                                                                   const BSONObj&) {
    return {};
}

void OpObserverNoop::onDelete(OperationContext*,
                              const NamespaceString&,
                              OptionalCollectionUUID,
                              CollectionShardingState::DeleteState,
                              bool) {}

void OpObserverNoop::onOpMessage(OperationContext*, const BSONObj&) {}

void OpObserverNoop::onCreateCollection(OperationContext*,
                                        Collection*,
                                        const NamespaceString&,
                                        const CollectionOptions&,
                                        const BSONObj&) {}

void OpObserverNoop::onCollMod(OperationContext*,
                               const NamespaceString&,
                               OptionalCollectionUUID,
                               const BSONObj&,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<TTLCollModInfo> ttlInfo) {}

void OpObserverNoop::onDropDatabase(OperationContext*, const std::string&) {}

repl::OpTime OpObserverNoop::onDropCollection(OperationContext*,
                                              const NamespaceString&,
                                              OptionalCollectionUUID) {
    return {};
}

void OpObserverNoop::onDropIndex(OperationContext*,
                                 const NamespaceString&,
                                 OptionalCollectionUUID,
                                 const std::string&,
                                 const BSONObj&) {}

void OpObserverNoop::onRenameCollection(OperationContext*,
                                        const NamespaceString&,
                                        const NamespaceString&,
                                        OptionalCollectionUUID,
                                        bool,
                                        OptionalCollectionUUID,
                                        OptionalCollectionUUID,
                                        bool) {}

void OpObserverNoop::onApplyOps(OperationContext*, const std::string&, const BSONObj&) {}

void OpObserverNoop::onConvertToCapped(OperationContext*,
                                       const NamespaceString&,
                                       OptionalCollectionUUID,
                                       double) {}

void OpObserverNoop::onEmptyCapped(OperationContext*,
                                   const NamespaceString&,
                                   OptionalCollectionUUID) {}

}  // namespace mongo
