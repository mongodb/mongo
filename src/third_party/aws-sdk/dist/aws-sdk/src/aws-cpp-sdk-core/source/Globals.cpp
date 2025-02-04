/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Api.h>
#include <aws/crt/io/TlsOptions.h>
#include <aws/crt/io/Bootstrap.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/auth/auth.h>

namespace Aws
{
    static const char TAG[] = "GlobalEnumOverflowContainer";

    static Aws::Crt::ApiHandle* g_apiHandle;
    static std::shared_ptr<Aws::Crt::Io::ClientBootstrap> g_defaultClientBootstrap(nullptr);
    static std::shared_ptr<Aws::Crt::Io::TlsConnectionOptions> g_defaultTlsConnectionOptions(nullptr);

    Aws::Crt::ApiHandle* GetApiHandle()
    {
        return g_apiHandle;
    }

    void SetDefaultClientBootstrap(const std::shared_ptr<Aws::Crt::Io::ClientBootstrap>& clientBootstrap)
    {
        g_defaultClientBootstrap = clientBootstrap;
    }

    Aws::Crt::Io::ClientBootstrap* GetDefaultClientBootstrap()
    {
        return g_defaultClientBootstrap.get();
    }

    void SetDefaultTlsConnectionOptions(const std::shared_ptr<Aws::Crt::Io::TlsConnectionOptions>& tlsConnectionOptions)
    {
        g_defaultTlsConnectionOptions = tlsConnectionOptions;
    }

    Aws::Crt::Io::TlsConnectionOptions* GetDefaultTlsConnectionOptions()
    {
        return g_defaultTlsConnectionOptions.get();
    }

    void InitializeCrt()
    {
        g_apiHandle = Aws::New<Aws::Crt::ApiHandle>(TAG, Aws::get_aws_allocator());
        AWS_FATAL_ASSERT(g_apiHandle);
        auto crtVersion = g_apiHandle->GetCrtVersion();
        AWS_LOGSTREAM_INFO(TAG, "Initialized AWS-CRT-CPP with version "
                << crtVersion.major << "." << crtVersion.minor << "." << crtVersion.patch);
    }

    void CleanupCrt()
    {
        Aws::SetDefaultClientBootstrap(nullptr);
        Aws::SetDefaultTlsConnectionOptions(nullptr);
        Aws::Delete(g_apiHandle);
        g_apiHandle = nullptr;
    }

    static Utils::EnumParseOverflowContainer* g_enumOverflow;

    Utils::EnumParseOverflowContainer* GetEnumOverflowContainer()
    {
        return g_enumOverflow;
    }

    void InitializeEnumOverflowContainer()
    {
        g_enumOverflow = Aws::New<Aws::Utils::EnumParseOverflowContainer>(TAG);
    }

    void CleanupEnumOverflowContainer()
    {
        Aws::Delete(g_enumOverflow);
        g_enumOverflow = nullptr;
    }
}
