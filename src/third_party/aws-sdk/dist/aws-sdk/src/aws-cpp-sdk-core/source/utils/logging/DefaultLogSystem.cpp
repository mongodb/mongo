/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/utils/logging/DefaultLogSystem.h>

#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

#include <fstream>

using namespace Aws::Utils;
using namespace Aws::Utils::Logging;

static const char* AllocationTag = "DefaultLogSystem";
static const int BUFFERED_MSG_COUNT = 100;

static std::shared_ptr<Aws::OFStream> MakeDefaultLogFile(const Aws::String& filenamePrefix)
{
    Aws::String newFileName = filenamePrefix + DateTime::CalculateGmtTimestampAsString("%Y-%m-%d-%H") + ".log";
    return Aws::MakeShared<Aws::OFStream>(AllocationTag, newFileName.c_str(), Aws::OFStream::out | Aws::OFStream::app);
}

static void LogThread(DefaultLogSystem::LogSynchronizationData* syncData, std::shared_ptr<Aws::OStream> logFile, const Aws::String& filenamePrefix, bool rollLog)
{
    // localtime requires access to env. variables to get Timezone, which is not thread-safe
    int32_t lastRolledHour = DateTime::Now().GetHour(false /*localtime*/);
    Aws::Vector<Aws::String> messages;
    messages.reserve(BUFFERED_MSG_COUNT);

    for(;;)
    {
        std::unique_lock<std::mutex> locker(syncData->m_logQueueMutex);
        syncData->m_queueSignal.wait(locker, [&](){ return syncData->m_stopLogging == true || syncData->m_queuedLogMessages.size() > 0; } );

        if (syncData->m_stopLogging && syncData->m_queuedLogMessages.size() == 0)
        {
            break;
        }

        std::swap(messages, syncData->m_queuedLogMessages);
        locker.unlock();

        if (messages.size() > 0)
        {
            if (rollLog)
            {
                // localtime requires access to env. variables to get Timezone, which is not thread-safe
                int32_t currentHour = DateTime::Now().GetHour(false /*localtime*/);
                if (currentHour != lastRolledHour)
                {
                    logFile = MakeDefaultLogFile(filenamePrefix);
                    lastRolledHour = currentHour;
                }
            }

            for (const auto& msg : messages)
            {
                (*logFile) << msg;
            }

            logFile->flush();
        }
        messages.clear();
        if(messages.capacity() > 2 * BUFFERED_MSG_COUNT)
        {
            messages.shrink_to_fit();
            messages.reserve(BUFFERED_MSG_COUNT);
        }
    }

    {
        std::unique_lock<std::mutex> locker(syncData->m_logQueueMutex);
        syncData->m_loggingThreadStopped = true;
        syncData->m_queueSignal.notify_one();
    }
}

DefaultLogSystem::DefaultLogSystem(LogLevel logLevel, const std::shared_ptr<Aws::OStream>& logFile) :
    Base(logLevel),
    m_syncData(),
    m_loggingThread()
{
    m_syncData.m_queuedLogMessages.reserve(BUFFERED_MSG_COUNT);
    m_loggingThread = std::thread(LogThread, &m_syncData, logFile, "", false);
}

DefaultLogSystem::DefaultLogSystem(LogLevel logLevel, const Aws::String& filenamePrefix) :
    Base(logLevel),
    m_syncData(),
    m_loggingThread()
{
    m_syncData.m_queuedLogMessages.reserve(BUFFERED_MSG_COUNT);
    m_loggingThread = std::thread(LogThread, &m_syncData, MakeDefaultLogFile(filenamePrefix), filenamePrefix, true);
}

DefaultLogSystem::~DefaultLogSystem()
{
    Stop();

    // explicitly wait for logging thread to finish
    {
        std::unique_lock<std::mutex> locker(m_syncData.m_logQueueMutex);
        if (!m_syncData.m_loggingThreadStopped)
        {
            m_syncData.m_queueSignal.wait_for(locker,
                                              std::chrono::milliseconds(500),
                                              [&](){ return m_syncData.m_loggingThreadStopped; });
        }
    }

    m_loggingThread.join();
}

void DefaultLogSystem::ProcessFormattedStatement(Aws::String&& statement)
{
    std::lock_guard<std::mutex> locker(m_syncData.m_logQueueMutex);
    if (m_syncData.m_stopLogging)
    {
        return;
    }
    m_syncData.m_queuedLogMessages.emplace_back(std::move(statement));
    if(m_syncData.m_queuedLogMessages.size() >= BUFFERED_MSG_COUNT)
    {
        m_syncData.m_queueSignal.notify_one();
    }
}

void DefaultLogSystem::Flush()
{
    std::lock_guard<std::mutex> locker(m_syncData.m_logQueueMutex);
    m_syncData.m_queueSignal.notify_one();
}

void DefaultLogSystem::Stop()
{
    FormattedLogSystem::Stop();
    Flush();

    {
        std::lock_guard<std::mutex> locker(m_syncData.m_logQueueMutex);
        m_syncData.m_stopLogging = true;
        m_syncData.m_queueSignal.notify_one();
    }
}

