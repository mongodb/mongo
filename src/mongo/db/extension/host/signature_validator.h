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
#include <string_view>
#include <utility>

#include <unistd.h>

namespace mongo::extension::host {

/**
 * ValidatedExtension represents the result of an extension's signature verification. During the
 * extension loading process, we must take special care to ensure the extension file we validate
 * is not tampered with before the extension is loaded. Callers of SignatureValidator must use the
 * path() method to obtain the path from which to load the extension.
 *
 * When ValidatedExtension is instantiated with a file descriptor, it acts as an RAII handle around
 * the file descriptor. This is the case when signature validation is enabled. SignatureValidator
 * opens the extension's file descriptor and hands the descriptor to a ValidatedExtension. Once the
 * extension is loaded succesfully, callers must call leakDescriptor(), which intentionaly keeps the
 * descriptor's open .so path() - a "/proc/self/fd/N" string - in use. This prevents the descriptor
 * number from being recycled during the lifetime of the loaded library.
 *
 * When ValidatedExtension is instantiated with a path, there is no descriptor to own and the
 * reported path is the originally provided path (i.e original on-disk path). This is the case when
 * signature validation is disabled.
 */
class ValidatedExtension {
    static constexpr std::string_view kProcFdPath = "/proc/self/fd/";

public:
    explicit ValidatedExtension(std::string path) : _path(std::move(path)) {}
    explicit ValidatedExtension(int fd)
        : _fd(fd), _path(std::string{kProcFdPath} + std::to_string(_fd)) {}

    ~ValidatedExtension() {
        _closeDescriptor();
    }

    ValidatedExtension(const ValidatedExtension&) = delete;
    ValidatedExtension& operator=(const ValidatedExtension&) = delete;

    ValidatedExtension(ValidatedExtension&& other) noexcept
        : _fd(std::exchange(other._fd, -1)), _path(std::move(other._path)) {}

    ValidatedExtension& operator=(ValidatedExtension&& other) noexcept {
        if (this != &other) {
            _closeDescriptor();
            _fd = std::exchange(other._fd, -1);
            _path = std::move(other._path);
        }
        return *this;
    }

    const std::string& path() const {
        return _path;
    }

    /**
     * Releases ownership of the descriptor without closing it, dismissing the RAII close.
     * The descriptor is intentionally leaked so path() stays valid - and
     * its number is never recycled by a later open() - for the lifetime of the loaded library.
     * dlopen caches loaded objects by the name passed to it, so reusing a "/proc/self/fd/N" number
     * would make it hand back a previously-loaded object instead of loading the new library.
     */
    void leakDescriptor() {
        _fd = -1;
    }

private:
    void _closeDescriptor() {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    int _fd = -1;
    std::string _path;
};

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
     * Validates the extension's detached signature and returns a ValidatedExtension.
     * ValidatedExtension reports the path that should be used to load the extension after signature
     * validation.
     *
     * 'extensionPath' is the on-disk path to the extension:
     *     - The path must end with '.so'.
     *     - The path must guaranteed to exist prior to calling this method.
     *     - The path must be a regular file, owned by a trusted user and not group/other-writable.
     *
     * If the signature is not validated successfully, an exception is thrown.
     *
     * When signature validation is enabled, this method avoids a time-of-check/time-of-use
     * window between verification and loading:
     *    1) We open the extension into a file descriptor
     *    2) Verify that descriptor can't be tampered with
     *    3) Verifies the signature against the inode descriptor's bytes
     *    4) Returns ValidatedExtension containing the descriptor.
     * The returned ValidatedExtension reports the "/proc/self/fd/N" path that resolves to the
     * pinned inode. This guarantees the bytes that were verified against the signature are the
     * bytes that get loaded into the process.
     *
     * Callers of this method must call leakDescriptor() once the extension is loaded so the inode
     * path stays valid and its number is never recycled for the lifetime of the loaded library.
     *
     * When signature validation is disabled, the returned ValidatedExtension reports the original
     * 'extensionPath' on-disk.
     */
    ValidatedExtension validateExtensionSignature(const std::string& extensionName,
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
