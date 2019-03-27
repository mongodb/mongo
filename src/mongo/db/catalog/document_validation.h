/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"

namespace mongo {
/**
 * If true, Collection should do no validation of writes from this OperationContext.
 *
 * Note that Decorations are value-constructed so this defaults to false.
 */
extern const OperationContext::Decoration<bool> documentValidationDisabled;

inline StringData bypassDocumentValidationCommandOption() {
    return "bypassDocumentValidation";
}

inline bool shouldBypassDocumentValidationForCommand(const BSONObj& cmdObj) {
    return cmdObj[bypassDocumentValidationCommandOption()].trueValue();
}

/**
 * Disables document validation on a single OperationContext while in scope.
 * Resets to original value when leaving scope so they are safe to nest.
 */
class DisableDocumentValidation {
    DisableDocumentValidation(const DisableDocumentValidation&) = delete;
    DisableDocumentValidation& operator=(const DisableDocumentValidation&) = delete;

public:
    DisableDocumentValidation(OperationContext* opCtx)
        : _opCtx(opCtx), _initialState(documentValidationDisabled(_opCtx)) {
        documentValidationDisabled(_opCtx) = true;
    }

    ~DisableDocumentValidation() {
        documentValidationDisabled(_opCtx) = _initialState;
    }

private:
    OperationContext* const _opCtx;
    const bool _initialState;
};

/**
 * Disables document validation while in scope if the constructor is passed true.
 */
class DisableDocumentValidationIfTrue {
public:
    DisableDocumentValidationIfTrue(OperationContext* opCtx, bool shouldDisableValidation) {
        if (shouldDisableValidation)
            _documentValidationDisabler.emplace(opCtx);
    }

private:
    boost::optional<DisableDocumentValidation> _documentValidationDisabler;
};
}
