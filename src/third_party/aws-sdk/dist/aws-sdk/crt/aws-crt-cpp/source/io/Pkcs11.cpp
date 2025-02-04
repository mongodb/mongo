/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/io/Pkcs11.h>

#include <aws/io/logging.h>
#include <aws/io/pkcs11.h>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            std::shared_ptr<Pkcs11Lib> Pkcs11Lib::Create(const String &filename, Allocator *allocator)
            {
                return Create(filename, InitializeFinalizeBehavior::Default, allocator);
            }

            std::shared_ptr<Pkcs11Lib> Pkcs11Lib::Create(
                const String &filename,
                InitializeFinalizeBehavior initializeFinalizeBehavior,
                Allocator *allocator)
            {
                aws_pkcs11_lib_options options;
                AWS_ZERO_STRUCT(options);

                if (!filename.empty())
                {
                    options.filename = ByteCursorFromString(filename);
                }

                switch (initializeFinalizeBehavior)
                {
                    case InitializeFinalizeBehavior::Default:
                        options.initialize_finalize_behavior = AWS_PKCS11_LIB_DEFAULT_BEHAVIOR;
                        break;
                    case InitializeFinalizeBehavior::Omit:
                        options.initialize_finalize_behavior = AWS_PKCS11_LIB_OMIT_INITIALIZE;
                        break;
                    case InitializeFinalizeBehavior::Strict:
                        options.initialize_finalize_behavior = AWS_PKCS11_LIB_STRICT_INITIALIZE_FINALIZE;
                        break;
                    default:
                        AWS_LOGF_ERROR(
                            AWS_LS_IO_PKCS11,
                            "Cannot create Pkcs11Lib. Invalid InitializeFinalizeBehavior %d",
                            (int)initializeFinalizeBehavior);
                        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                        return nullptr;
                }

                struct aws_pkcs11_lib *impl = aws_pkcs11_lib_new(allocator, &options);
                if (impl == nullptr)
                {
                    return nullptr;
                }

                return MakeShared<Pkcs11Lib>(allocator, *impl);
            }

            Pkcs11Lib::Pkcs11Lib(aws_pkcs11_lib &impl) : impl(&impl) {}

            Pkcs11Lib::~Pkcs11Lib()
            {
                aws_pkcs11_lib_release(impl);
            }

        } // namespace Io
    } // namespace Crt
} // namespace Aws
