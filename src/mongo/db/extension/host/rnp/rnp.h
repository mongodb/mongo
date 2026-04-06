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

#include "mongo/util/modules.h"

#include <filesystem>
#include <string>

#include <rnp/rnp.h>

namespace mongo::extension::host::rnp {

/**
 * RnpInput is a wrapper around an rnp_input_t, a type that contains input content for RNP
 * context operations. An RnpInput can be created from a data file or from an in-memory data
 * buffer.
 */
class RnpInput {
public:
    RnpInput() = default;
    ~RnpInput();

    rnp_input_t operator*() const {
        return _input;
    }
    /**
     * Creates an RnpInput from a file located at the provided path. The provided path must be an
     * absolute path to the file location.
     */
    static RnpInput createFromPath(const std::filesystem::path& absolutePath);
    /**
     * Creates an RnpInput from a std::string. If doCopy is false, the provided string must remain
     * in scope for the lifetime of the returned RnpInput object. If this condition can't be
     * guaranteed, doCopy should be set to true, which will copy the contents and store them as part
     * of the rnp_input_t.
     */
    static RnpInput createFromMemory(const std::string& contents, bool doCopy = false);

private:
    rnp_input_t _input{nullptr};
};

/**
 * RnpContext is a wrapper around the RNP context. PGP validation operations can be be performed on
 * this context object once it is initialized. Note, RnpContext currently only supports importing a
 * single key into the keyring. This is because extensions are only expected to be signed with a
 * single key.
 *
 * RnpContext's intended usage is as follows:
 *
 * 1) Construct the context object.
 * 2) Initialize the context: ctx.initialize()
 * 3) Import key into the context: ctx.importKey()
 * 4) Verify a detached signature using the context (i.e keyring): ctx.verifyDetachedSignature()
 *
 * The same RnpContext can be used to verify multiple signatures against the key in the keyring. The
 * RnpContext must always be initialized first before any other operations are performed on it.
 * Initalization could have been done in the constructor, however, in some cases, such as in our
 * SignatureValidator, the RnpContext can be conditionally initialized, so we opted to initialize()
 * a separate method on the RnpContext's interface.
 */
class RnpContext {
public:
    RnpContext() = default;
    ~RnpContext();
    /**
     * initialize initializes the rnp_ffi_t, which we refer to as the RNP context/keyring. This
     * method must be called before any other operations can be performed on the RnpContext.
     */
    void initialize();
    rnp_ffi_t operator*() const;
    /**
     * Imports the provided key into the RnpContext (i.e keyring). This key does not persist on the
     * file system, and lives entirely in memory within the scope of the RnpContext.
     */
    void importKey(const RnpInput& key);
    /**
     * verifyDetachedSignature asserts that the detached signature (signatureFilePath) was generated
     * with a key that has been imported into this RnpContext (i.e keyring). Note, this method
     * throws if it fails to validate the signature.
     */
    void verifyDetachedSignature(const std::string& signedDataFilePath,
                                 const std::string& signatureFilePath) const;

private:
    rnp_ffi_t _ffi{nullptr};
    size_t _numImportedKeys{0};
};

/**
 * RnpOutput is a wrapper around an rnp_output_t, a type used by RNP context as a data output. An
 * rnp_output_t can be backed by a data buffer or a file. Here, we only provide an implementation
 * for the data buffer backed output. Note, this class is only used from our unit tests in order to
 * verify the mongot extension signing key is read correctly in secure mode. This is because we are
 * unable to test signature verification in unit tests directly since the mongot extension is not
 * available within signature_validator_test.cpp.
 */
class RnpOutput {
public:
    RnpOutput();
    ~RnpOutput();

    rnp_output_t operator*() const {
        return _output;
    }
    /**
     * Reads the contents of the RnpInput into this RnpOutput object.
     */
    void pipeFromInput(const RnpInput& input);

    /**
     * Returns the data contents of the RnpOutput as a string_view.
     */
    std::string_view getAsStringView() const;

private:
    rnp_output_t _output{nullptr};
    uint8_t* _buf{nullptr};
    size_t _bufLen{0};
};
}  // namespace mongo::extension::host::rnp
