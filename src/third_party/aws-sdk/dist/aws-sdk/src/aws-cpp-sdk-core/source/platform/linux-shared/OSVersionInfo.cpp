/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/platform/OSVersionInfo.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/StringUtils.h>
#include <sys/utsname.h>

namespace Aws
{
namespace OSVersionInfo
{

Aws::String GetSysCommandOutput(const char* command)
{
    Aws::String outputStr;
    FILE* outputStream;
    const int maxBufferSize = 256;
    char outputBuffer[maxBufferSize];

    outputStream = popen(command, "r");

    if (outputStream)
    {
        while (!feof(outputStream))
        {
            if (fgets(outputBuffer, maxBufferSize, outputStream) != nullptr)
            {
                outputStr.append(outputBuffer);
            }
        }

        pclose(outputStream);

        return Aws::Utils::StringUtils::Trim(outputStr.c_str());
    }

    return {};
}


Aws::String ComputeOSVersionString()
{
    utsname name;
    int32_t success = uname(&name);
    if(success >= 0)
    {
        Aws::StringStream ss;
        ss << name.sysname << "/" << name.release;
        return ss.str();
    }

    return "other";
}

Aws::String ComputeOSVersionArch()
{
    utsname name;
    int32_t success = uname(&name);
    if(success >= 0)
    {
        Aws::StringStream ss;
        ss << name.machine;
        return ss.str();
    }

    return "";
}

} // namespace OSVersionInfo
} // namespace Aws
