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

inline StringData bypassDocumentValidationCommandOption() {
    return "bypassDocumentValidation";
}

inline bool shouldBypassDocumentValidationForCommand(const BSONObj& cmdObj) {
    return cmdObj[bypassDocumentValidationCommandOption()].trueValue();
}

/**
 * This container decorates an OperationContext object. It stores the document validation
 * settings for writes associated with an OperationContext. By default, document validation (both
 * schema and internal) is enabled. DocumentValidationSettings objects are not thread-safe.
 *
 */
class DocumentValidationSettings {
public:
    enum flag : std::uint8_t {
        /*
         * Enables document validation (both schema and internal).
         */
        kEnableValidation = 0x00,
        /*
         * Disables the schema validation during document inserts and updates.
         * This flag should be enabled if WriteCommandBase::_bypassDocumentValidation
         * is set to true.
         */
        kDisableSchemaValidation = 0x01,
        /*
         * Disables any internal validation (like fixDocumentForInsert()). This flag
         * should be enabled only for trusted internal writes or internal writes that
         * doesn't comply with internal validation rules.
         */
        kDisableInternalValidation = 0x02,
    };

    using Flags = std::uint8_t;

    static const OperationContext::Decoration<DocumentValidationSettings> get;

    DocumentValidationSettings() = default;

    void setFlags(Flags flags) {
        invariant(flags != kEnableValidation);
        _flags |= flags;
    }

    void clearFlags() {
        _flags = kEnableValidation;
    }

    bool isSchemaValidationDisabled() const {
        return _flags & kDisableSchemaValidation;
    }

    bool isInternalValidationDisabled() const {
        return _flags & kDisableInternalValidation;
    }

    bool isDocumentValidationEnabled() const {
        return _flags == kEnableValidation;
    }

private:
    Flags _flags = kEnableValidation;
};

/**
 * Disables document validation on a single OperationContext while in scope.
 * Resets to original value when leaving scope so they are safe to nest.
 */
class DisableDocumentValidation {
    DisableDocumentValidation(const DisableDocumentValidation&) = delete;
    DisableDocumentValidation& operator=(const DisableDocumentValidation&) = delete;

public:
    DisableDocumentValidation(OperationContext* opCtx,
                              DocumentValidationSettings::Flags flags =
                                  DocumentValidationSettings::kDisableSchemaValidation)
        : _opCtx(opCtx) {
        auto& documentValidationSettings = DocumentValidationSettings::get(_opCtx);
        _initialState = documentValidationSettings;
        documentValidationSettings.setFlags(flags);
    }

    ~DisableDocumentValidation() {
        DocumentValidationSettings::get(_opCtx) = _initialState;
    }

private:
    OperationContext* const _opCtx;
    DocumentValidationSettings _initialState;
};

/**
 * Disables document schema validation while in scope if the constructor is passed true.
 */
class DisableDocumentSchemaValidationIfTrue {
public:
    DisableDocumentSchemaValidationIfTrue(OperationContext* opCtx,
                                          bool shouldDisableSchemaValidation) {
        if (shouldDisableSchemaValidation)
            _documentSchemaValidationDisabler.emplace(opCtx);
    }

private:
    boost::optional<DisableDocumentValidation> _documentSchemaValidationDisabler;
};
}  // namespace mongo
