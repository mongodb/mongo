/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "qpl/qpl.h"

namespace qpl {

auto get_library_version() -> const char * {
    return QPL_VERSION;
}

}

extern "C" const char *qpl_get_library_version() {
    return qpl::get_library_version();
}
