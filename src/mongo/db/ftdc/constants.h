// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

extern const char kFTDCInterimFile[];
extern const char kFTDCInterimTempFile[];
extern const char kFTDCArchiveFile[];

extern const char kFTDCIdField[];
extern const char kFTDCTypeField[];

extern const char kFTDCDataField[];
extern const char kFTDCDocField[];

extern const char kFTDCDocsField[];

extern const char kFTDCCollectStartField[];
extern const char kFTDCCollectEndField[];

constexpr std::string_view kFTDCDefaultDirectory = "diagnostic.data"sv;

}  // namespace mongo
