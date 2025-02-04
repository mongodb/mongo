/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/FileSystemUtils.h>

using namespace Aws::Utils;

Aws::String PathUtils::GetFileNameFromPathWithoutExt(const Aws::String& path)
{
    Aws::String fileName = Aws::Utils::PathUtils::GetFileNameFromPathWithExt(path);
    size_t endPos = fileName.find_last_of('.');
    if (endPos == std::string::npos)
    {
        return fileName;
    } 
    if (endPos == 0) // fileName is "."
    {
        return {};
    }

    return fileName.substr(0, endPos);
}

Aws::String PathUtils::GetFileNameFromPathWithExt(const Aws::String& path)
{
    if (path.size() == 0) 
    {	
        return path;
    }

    size_t startPos = path.find_last_of(Aws::FileSystem::PATH_DELIM);
    if (startPos == path.size() - 1)
    {
        return {};
    }

    if (startPos == std::string::npos)
    {
        startPos = 0;
    }
    else 
    {
        startPos += 1;
    }

    size_t endPos = path.size() - 1;
    
    return path.substr(startPos, endPos - startPos + 1);
}
