/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/BinToken.h"

#include <sys/types.h>

namespace js {
namespace frontend {

const char* BINKIND_DESCRIPTIONS[] = {
#define WITH_KIND(_, SPEC_NAME) #SPEC_NAME,
    FOR_EACH_BIN_KIND(WITH_KIND)
#undef WITH_KIND
};

const char* BINFIELD_DESCRIPTIONS[] = {
    #define WITH_FIELD(_, SPEC_NAME) #SPEC_NAME,
        FOR_EACH_BIN_FIELD(WITH_FIELD)
    #undef WITH_FIELD
};

const char* describeBinKind(const BinKind& kind) {
    return BINKIND_DESCRIPTIONS[static_cast<size_t>(kind)];
}

const char* describeBinField(const BinField& field) {
    return BINFIELD_DESCRIPTIONS[static_cast<size_t>(field)];
}

} // namespace frontend
} // namespace js
