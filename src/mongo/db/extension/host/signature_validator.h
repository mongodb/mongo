/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/extension/host/rnp/rnp.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo::extension::host {
/**
 * SignatureValidator is responsible for validating an extension's signature file against a
 * public key.
 *
 * This class respects the compile-time pre-processor flag MONGO_CONFIG_EXT_SIG_SECURE and
 * server options (i.e extensionsSignaturePublicKeyPath) when determining which validation
 * public key to use for signature verification. Note, this class is always safe to instantiate,
 * even if signature verification is disabled (i.e extensionsSignaturePublicKeyPath is empty).
 */
class SignatureValidator {
public:
    SignatureValidator();
    SignatureValidator(const SignatureValidator&) = delete;
    SignatureValidator& operator=(const SignatureValidator&) = delete;

    virtual ~SignatureValidator();
    /**
     * Validates the extension's detached signature file against the validation public key.
     * Note, extensionPath must be guaranteed to exist prior to calling this method. If the
     * signature is not validated succesfully, an exception is thrown. extensionName must be the
     * extension's file name including the '.so' suffix.
     */
    void validateExtensionSignature(const std::string& extensionName,
                                    const std::string& extensionPath) const;

protected:
    /**
     * The value of secure mode is determined by the MONGO_CONFIG_EXT_SIG_SECURE macro. However,
     * for unit testing, we want to be able to control being in secure mode or not directly in test
     * logic, so this protected constructor is provided to accomodate that. In unit testing,
     * SignatureValidator is constructed directly using this constructor. Alternatively, the server
     * logic uses the argument-less constructor, which delegates to this constructor, using the
     * value of MONGO_CONFIG_EXT_SIG_SECURE to determine the secureMode argument value.
     */
    SignatureValidator(bool secureMode);


    /**
     * This method is made protected in order to allow us to unit test that the secure mode public
     * key is used to generate the RnpInput correctly.
     */
    rnp::RnpInput _getValidationPublicKeyAsRnpInput() const;

    /**
     * Both _secureMode and _skipValidation are deliberately made const, and should not be made
     * mutable in the future unless SignatureValidator undergoes significant behavioural changes.
     *
     * With the exception of unit tests, _secureMode is determined at compilation time by the
     * MONGO_CONFIG_EXT_SIG_SECURE macro and should never change at runtime.
     * _skipValidation is derived from _secureMode at instantiation time, and should not be changed
     * during the object's lifetime. This is because the RnpContext (i.e keyring) is conditionally
     * initialized at instantiation time based on the value of _skipValidation, so any changes to
     * the value of _skipValidation would leave the SignatureValidator in an inconsistent state.
     */
    const bool _secureMode;
    const bool _skipValidation;

private:
    rnp::RnpContext _rnpCtx;
};

}  // namespace mongo::extension::host
