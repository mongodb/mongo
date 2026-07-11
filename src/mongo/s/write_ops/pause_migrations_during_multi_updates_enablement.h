// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

class PauseMigrationsDuringMultiUpdatesEnablement {
public:
    bool isEnabled();

private:
    boost::optional<bool> _enabled;
};

}  // namespace mongo
