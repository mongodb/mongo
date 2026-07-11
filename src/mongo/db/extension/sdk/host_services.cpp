// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/host_services.h"

#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"

namespace mongo::extension::sdk {

// The static handle is initially set to nullptr. It will be set "for real" at the
//  very start of extension initialization, before any extension should attempt to access it.
// TODO: This static pointer should be able to initialize inline, since extensions should build
// statically.
UnownedHandle<const ::MongoExtensionHostServices> HostServicesAPI::_sHostServices{nullptr};

void HostServicesAPI::assertVTableConstraints(const VTable_t& vtable) {
    sdk_tassert(
        11097801, "Host services' 'user_asserted' is null", vtable.user_asserted != nullptr);
    sdk_tassert(11338300, "Host services' 'get_logger' is null", vtable.get_logger != nullptr);
    // Note that we intentionally do not validate tripwire_asserted here. If it wasn't valid, the
    // tripwire assert would fire and we would dereference the nullptr anyway.
    sdk_tassert(11149304,
                "Host services' 'create_host_agg_stage_parse_node' is null",
                vtable.create_host_agg_stage_parse_node != nullptr);
    sdk_tassert(
        11134201, "Host services' 'create_id_lookup' is null", vtable.create_id_lookup != nullptr);
    sdk_tassert(12601500,
                "Host services' 'create_document_results_and_metadata' is null",
                vtable.create_document_results_and_metadata != nullptr);
}

}  // namespace mongo::extension::sdk
