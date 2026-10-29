// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

class CursorInitialReply;
class AnyCursor;

/**
 * Function used by the IDL parser to validate that a response has exactly one cursor type field.
 */
void validateIDLParsedCursorResponse(const CursorInitialReply* idlParsedObj);

/**
 * Function used by the IDL parser to verify that a response cursor has a firstBatch or nextBatch.
 */
void validateIDLParsedAnyCursor(const AnyCursor* idlParsedObj);
}  // namespace mongo
