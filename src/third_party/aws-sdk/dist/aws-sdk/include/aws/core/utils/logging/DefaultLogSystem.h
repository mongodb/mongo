/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/logging/FormattedLogSystem.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>

#include <thread>
#include <memory>
#include <mutex>
#include <atomic>
#include <condition_variable>

namespace Aws
{
    namespace Utils
    {
        namespace Logging
        {
            /**
             * Default behavior logger. It has a background thread that reads a log queue and prints the messages
             * out to file as quickly as possible.  This implementation also rolls the file every hour.
             */
            class AWS_CORE_API DefaultLogSystem : public FormattedLogSystem
            {
            public:
                using Base = FormattedLogSystem;

                /**
                 * Initialize the logging system to write to the supplied logfile output. Creates logging thread on construction.
                 */
                DefaultLogSystem(LogLevel logLevel, const std::shared_ptr<Aws::OStream>& logFile);
                /**
                 * Initialize the logging system to write to a computed file path filenamePrefix + "timestamp.log". Creates logging thread
                 * on construction.
                 */
                DefaultLogSystem(LogLevel logLevel, const Aws::String& filenamePrefix);

                virtual ~DefaultLogSystem();

                /**
                 * Flushes buffered messages to the file system.
                 * This method is thread-safe.
                 */
                void Flush() override;

                /**
                 * Stops logging on this logger without destroying the object.
                 */
                void Stop() override;

                /**
                 * Structure containing semaphores, queue etc... 
                 */
                struct LogSynchronizationData
                {
                public:
                    LogSynchronizationData() : m_stopLogging(false), m_loggingThreadStopped(false) {}

                    std::mutex m_logQueueMutex;
                    std::condition_variable m_queueSignal;
                    Aws::Vector<Aws::String> m_queuedLogMessages;
                    bool m_stopLogging = false;
                    bool m_loggingThreadStopped = false;

                private:
                    LogSynchronizationData(const LogSynchronizationData& rhs) = delete;
                    LogSynchronizationData& operator =(const LogSynchronizationData& rhs) = delete;
                };

            protected:
                /**
                 * Pushes log onto the queue and notifies the background thread.
                 */
                virtual void ProcessFormattedStatement(Aws::String&& statement) override;

            private:
                DefaultLogSystem(const DefaultLogSystem& rhs) = delete;
                DefaultLogSystem& operator =(const DefaultLogSystem& rhs) = delete;

                LogSynchronizationData m_syncData;

                std::thread m_loggingThread;
            };

        } // namespace Logging
    } // namespace Utils
} // namespace Aws
