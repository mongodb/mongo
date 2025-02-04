/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/logging/FormattedLogSystem.h>

namespace Aws
{
    namespace Utils
    {
        namespace Logging
        {
            /**
             * Log system interface that logs to std::cout
             */
            class AWS_CORE_API ConsoleLogSystem : public FormattedLogSystem
            {
            public:

                using Base = FormattedLogSystem;

                ConsoleLogSystem(LogLevel logLevel) :
                    Base(logLevel)
                {}

                virtual ~ConsoleLogSystem() {}

                /**
                 * Flushes buffered messages to stdout.
                 */
                void Flush() override;

            protected:

                virtual void ProcessFormattedStatement(Aws::String&& statement) override;
            };

        } // namespace Logging
    } // namespace Utils
} // namespace Aws
