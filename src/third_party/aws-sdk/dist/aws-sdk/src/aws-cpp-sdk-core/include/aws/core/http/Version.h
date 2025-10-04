/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

namespace Aws {
    namespace Http {
        /**
         * Enum to represent version of the http protocol to use
         */

#ifdef _WIN32
#pragma push_macro("WIN_HTTP_VERSION")
#undef HTTP_VERSION_1_0
#undef HTTP_VERSION_1_1
#undef HTTP_VERSION_2_0

#pragma pop_macro("WIN_HTTP_VERSION")
#endif //#ifdef _WIN32
        enum class Version {
            HTTP_VERSION_NONE,
            HTTP_VERSION_1_0,
            HTTP_VERSION_1_1,
            HTTP_VERSION_2_0,
            HTTP_VERSION_2TLS,
            HTTP_VERSION_2_PRIOR_KNOWLEDGE,
            HTTP_VERSION_3,
            HTTP_VERSION_3ONLY,
        };
    }
}
