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

#include "mongo/db/query/virtual_collection/external_record_store.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/query/virtual_collection/multi_bson_stream_cursor.h"
#include "mongo/db/storage/record_store.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

std::variant<ExternalIntegerKeyedContainer, ExternalStringKeyedContainer>
ExternalRecordStore::_makeContainer() {
    auto container = ExternalIntegerKeyedContainer();
    return container;
}
// 'ident' is an identifer to WT table and a virtual collection does not have any persistent data
// in WT. So, we set the "dummy" ident for a virtual collection.
ExternalRecordStore::ExternalRecordStore(boost::optional<UUID> uuid,
                                         const VirtualCollectionOptions& vopts)
    : _vopts(vopts), _container(_makeContainer()) {}

/**
 * Returns a MultiBsonStreamCursor for this record store. Reverse scans are not currently supported
 * for this record store type, so if 'forward' is false this asserts.
 */
std::unique_ptr<SeekableRecordCursor> ExternalRecordStore::getCursor(OperationContext* opCtx,
                                                                     RecoveryUnit& ru,
                                                                     bool forward) const {
    if (forward) {
        return std::make_unique<MultiBsonStreamCursor>(getOptions());
    }
    tasserted(6968302, "MultiBsonStreamCursor does not support reverse scans");
    return nullptr;
}
RecordStore::RecordStoreContainer ExternalRecordStore::getContainer() {
    return std::visit([](auto& v) -> RecordStore::RecordStoreContainer { return v; }, _container);
}

}  // namespace mongo
