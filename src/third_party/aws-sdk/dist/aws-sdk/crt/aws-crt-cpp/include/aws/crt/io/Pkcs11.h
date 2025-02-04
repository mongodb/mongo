#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Types.h>

struct aws_pkcs11_lib;

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            /**
             * Handle to a loaded PKCS#11 library.
             *
             * For most use cases, a single instance of Pkcs11Lib should be used for the
             * lifetime of your application.
             */
            class AWS_CRT_CPP_API Pkcs11Lib
            {
              public:
                /**
                 * Controls how Pkcs11Lib calls `C_Initialize()` and `C_Finalize()`
                 * on the PKCS#11 library.
                 */
                enum class InitializeFinalizeBehavior
                {
                    /**
                     * Default behavior that accommodates most use cases.
                     *
                     * `C_Initialize()` is called on creation, and "already-initialized"
                     * errors are ignored. `C_Finalize()` is never called, just in case
                     * another part of your application is still using the PKCS#11 library.
                     */
                    Default,

                    /**
                     * Skip calling `C_Initialize()` and `C_Finalize()`.
                     *
                     * Use this if your application has already initialized the PKCS#11 library, and
                     * you do not want `C_Initialize()` called again.
                     */
                    Omit,

                    /**
                     * `C_Initialize()` is called on creation and `C_Finalize()` is
                     * called on cleanup.
                     *
                     * If `C_Initialize()` reports that's it's already initialized, this is
                     * treated as an error. Use this if you need perfect cleanup (ex: running
                     * valgrind with --leak-check).
                     */
                    Strict,
                };

                /**
                 * Load and initialize a PKCS#11 library.
                 *
                 * `C_Initialize()` and `C_Finalize()` are called on the PKCS#11
                 * library in the InitializeFinalizeBehavior::Default way.
                 *
                 * @param filename Name or path of PKCS#11 library file to load (UTF-8).
                 *                 Pass an empty string if your application already has PKCS#11 symbols linked in.
                 *
                 * @param allocator Memory allocator to use.
                 *
                 * @return If successful a `shared_ptr` containing the Pkcs11Lib is returned.
                 *         If unsuccessful the `shared_ptr` will be empty, and Aws::Crt::LastError()
                 *         will contain the error that occurred.
                 */
                static std::shared_ptr<Pkcs11Lib> Create(const String &filename, Allocator *allocator = ApiAllocator());

                /**
                 * Load a PKCS#11 library, specifying how `C_Initialize()` and `C_Finalize()` will be called.
                 *
                 * @param filename Name or path of PKCS#11 library file to load (UTF-8).
                 *                 Pass an empty string if your application already has PKCS#11 symbols linked in.
                 *
                 * @param initializeFinalizeBehavior Specifies how `C_Initialize()` and
                 *                                   `C_Finalize()` will be called on the
                 *                                   PKCS#11 library.
                 * @param allocator Memory allocator to use.
                 *
                 * @return If successful a `shared_ptr` containing the Pkcs11Lib is returned.
                 *         If unsuccessful the `shared_ptr` will be empty, and Aws::Crt::LastError()
                 *         will contain the error that occurred.
                 */
                static std::shared_ptr<Pkcs11Lib> Create(
                    const String &filename,
                    InitializeFinalizeBehavior initializeFinalizeBehavior,
                    Allocator *allocator = ApiAllocator());

                ~Pkcs11Lib();

                /// @private
                aws_pkcs11_lib *GetNativeHandle() { return impl; }

                /// @private Use Create(...), this constructor is for internal use only
                explicit Pkcs11Lib(aws_pkcs11_lib &impl);

              private:
                // no copy/move
                Pkcs11Lib(const Pkcs11Lib &) = delete;
                Pkcs11Lib(Pkcs11Lib &&) = delete;
                Pkcs11Lib &operator=(const Pkcs11Lib &) = delete;
                Pkcs11Lib &operator=(Pkcs11Lib &&) = delete;

                aws_pkcs11_lib *impl = nullptr;
            };
        } // namespace Io
    } // namespace Crt
} // namespace Aws
