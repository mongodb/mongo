// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/util/modules.h"

#include <ostream>

namespace mongo::sbe::vm {

class CodeFragmentPrinter {
public:
    CodeFragmentPrinter(CodeFragment::PrintFormat format) : _format(format) {}

    void print(std::ostream& os, const CodeFragment& code) const;

private:
    CodeFragment::PrintFormat _format;
};
}  // namespace mongo::sbe::vm
