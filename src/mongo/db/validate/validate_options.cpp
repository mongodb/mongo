// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/validate_options.h"

namespace mongo::collection_validation {

ValidationOptions::ValidationOptions(ValidateMode validateMode,
                                     RepairMode repairMode,
                                     bool logDiagnostics,
                                     ValidationVersion validationVersion,
                                     boost::optional<std::string> verifyConfigurationOverride,
                                     boost::optional<Timestamp> readTimestamp,
                                     boost::optional<std::vector<std::string>> hashPrefixes,
                                     boost::optional<std::vector<std::string>> revealHashedIds)
    : _validateMode(validateMode),
      _repairMode(repairMode),
      _logDiagnostics(logDiagnostics),
      _validationVersion(validationVersion),
      _verifyConfigurationOverride(std::move(verifyConfigurationOverride)),
      _readTimestamp(readTimestamp),
      _hashPrefixes(std::move(hashPrefixes)),
      _revealHashedIds(std::move(revealHashedIds)) {}

}  // namespace mongo::collection_validation
