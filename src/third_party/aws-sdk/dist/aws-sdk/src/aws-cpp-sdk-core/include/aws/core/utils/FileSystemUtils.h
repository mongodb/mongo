/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once
#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/platform/FileSystem.h>

#include <fstream>

namespace Aws
{
    namespace Utils
    {

        class AWS_CORE_API FStreamWithFileName : public Aws::FStream
        {
            public:
                FStreamWithFileName(const Aws::String& fileName, std::ios_base::openmode openFlags) :
                        Aws::FStream(fileName.c_str(), openFlags), m_fileName(fileName) {}

                virtual ~FStreamWithFileName() = default;

                const Aws::String& GetFileName() const { return m_fileName; }
            protected:
                Aws::String m_fileName;
        };

        /**
         *  Provides a fstream around a temporary file. This file gets deleted upon an instance of this class's destructor being called.
         */
        class AWS_CORE_API TempFile : public Aws::Utils::FStreamWithFileName
        {
        public:
            /**
             *  Creates a temporary file with [prefix][temp name][suffix] e.g.
             *  prefix of "foo" and suffix of ".bar" will generate foo[some random string].bar
             */
            TempFile(const char* prefix, const char* suffix, std::ios_base::openmode openFlags);
            /**
            *  Creates a temporary file with [prefix][temp name] e.g.
            *  prefix of "foo" will generate foo[some random string]
            */
            TempFile(const char* prefix, std::ios_base::openmode openFlags);
            /**
             * Creates a temporary file with a random string for the name.
             */
            TempFile(std::ios_base::openmode openFlags);

            ~TempFile();
        };

        class AWS_CORE_API PathUtils
        {
        public:

            /**
             * Get file name from it's full path, e.g get "file1" from "/path/to/file1.ext"; get "file2" from "/path/to/file2"
             */
            static Aws::String GetFileNameFromPathWithoutExt(const Aws::String& path);

            /**
             * Get file name from it's full path, e.g get "file1.ext" from "/path/to/file1.ext"; get "file2" from "/path/to/file2"
             */
            static Aws::String GetFileNameFromPathWithExt(const Aws::String& path);
        };
    }
}
