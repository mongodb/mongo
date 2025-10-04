/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/FileSystemUtils.h>

#include <aws/core/platform/FileSystem.h>

namespace Aws
{
    namespace Utils
    {
        static Aws::String ComputeTempFileName(const char* prefix, const char* suffix)
        {
            Aws::String prefixStr;

            if (prefix)
            {
                prefixStr = prefix;
            }

            Aws::String suffixStr;

            if (suffix)
            {
                suffixStr = suffix;
            }

            return prefixStr + Aws::FileSystem::CreateTempFilePath() + suffixStr;
        }

        TempFile::TempFile(const char* prefix, const char* suffix, std::ios_base::openmode openFlags) :
            FStreamWithFileName(ComputeTempFileName(prefix, suffix).c_str(), openFlags)
        {
        }

        TempFile::TempFile(const char* prefix, std::ios_base::openmode openFlags) :
            FStreamWithFileName(ComputeTempFileName(prefix, nullptr).c_str(), openFlags)
        {
        }

        TempFile::TempFile(std::ios_base::openmode openFlags) :
            FStreamWithFileName(ComputeTempFileName(nullptr, nullptr).c_str(), openFlags)
        {
        }


        TempFile::~TempFile()
        {
            Aws::FileSystem::RemoveFileIfExists(m_fileName.c_str());
        }
    }
}