// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Represents options passed to the explain command (aside from the command which is being explained
 * and its parameters).
 */
class [[MONGO_MOD_PUBLIC]] ExplainOptions {
public:
    /**
     * The various supported verbosity levels for explain. The order is significant: the enum values
     * are assigned in order of increasing verbosity.
     */
    using Verbosity = explain::VerbosityEnum;

    static constexpr std::string_view kVerbosityName = "verbosity"sv;

    /**
     * Returns true if 'verbosity' is one of the "version 3" (V3) explain verbosity modes. When any
     * of these is requested, explain reports "explainVersion: '3'".
     */
    static bool isV3Verbosity(ExplainOptions::Verbosity verbosity);

    /**
     * Converts an explain verbosity to its string representation.
     */
    static std::string_view verbosityString(ExplainOptions::Verbosity verbosity);

    /**
     * Converts 'verbosity' to its corresponding representation as a BSONObj containing explain
     * command parameters.
     */
    static BSONObj toBSON(ExplainOptions::Verbosity verbosity);
};

}  // namespace mongo
