// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/virtual_collection/external_record_store.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/query/virtual_collection/multi_bson_stream_cursor.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/stub_container.h"

#include <boost/optional/optional.hpp>

namespace mongo {

std::variant<StubIntegerKeyedContainer, StubStringKeyedContainer>
ExternalRecordStore::_makeContainer() {
    auto container = StubIntegerKeyedContainer();
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
