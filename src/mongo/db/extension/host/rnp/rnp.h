// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <filesystem>
#include <string>
#include <string_view>

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

    RnpInput(const RnpInput&) = delete;
    RnpInput& operator=(const RnpInput&) = delete;
    RnpInput(RnpInput&& other) noexcept;
    RnpInput& operator=(RnpInput&& other) noexcept;

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

    RnpContext(const RnpContext&) = delete;
    RnpContext& operator=(const RnpContext&) = delete;
    RnpContext(RnpContext&&) noexcept = delete;
    RnpContext& operator=(RnpContext&&) noexcept = delete;

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

    RnpOutput(const RnpOutput&) = delete;
    RnpOutput& operator=(const RnpOutput&) = delete;
    RnpOutput(RnpOutput&&) noexcept = delete;
    RnpOutput& operator=(RnpOutput&&) noexcept = delete;

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
