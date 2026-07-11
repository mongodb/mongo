// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/options_parser/startup_options.h"

#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"

#include <memory>

namespace mongo {
namespace optionenvironment {

OptionSection startupOptions("Options");
Environment startupOptionsParsed;

}  // namespace optionenvironment
}  // namespace mongo
