/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/component-registry/ComponentRegistry.h>

#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/LogMacros.h>

#include <aws/core/utils/memory/stl/AWSMap.h>
#include <mutex>

namespace Aws
{
    namespace Utils
    {
        namespace ComponentRegistry
        {
            const char RegistryTag[] = "ComponentRegistryAllocTag";

            struct ComponentDescriptor
            {
                const char* name;
                ComponentTerminateFn TerminateFn;
            };
            using Registry = Aws::UnorderedMap<void*, ComponentDescriptor>;

            static Registry* s_registry(nullptr);
            static std::mutex s_registryMutex;


            void InitComponentRegistry()
            {
                std::unique_lock<std::mutex> lock(s_registryMutex);
                assert(!s_registry);
                
                s_registry = Aws::New<Registry>(RegistryTag);
            }

            void ShutdownComponentRegistry()
            {
                std::unique_lock<std::mutex> lock(s_registryMutex);

                Aws::Delete(s_registry);
                s_registry = nullptr;
            }

            void RegisterComponent(const char* clientName, void* pClient, ComponentTerminateFn terminateMethod)
            {
                std::unique_lock<std::mutex> lock(s_registryMutex);
                assert(s_registry);
                assert(pClient);

                (*s_registry)[pClient] = ComponentDescriptor{clientName, terminateMethod};
            }

            void DeRegisterComponent(void* pClient)
            {
                std::unique_lock<std::mutex> lock(s_registryMutex);
                if (!s_registry)
                {
                    AWS_LOGSTREAM_ERROR(RegistryTag, "Attempt to de-register a component while registry is not initialized (or already terminated).\n"
                                                     "This is likely a call from a client destructor that outlived InitAPI(){...}ShutdownAPI() scope.\n"
                                                     "Please refer to "
                                                     "https://docs.aws.amazon.com/sdk-for-cpp/v1/developer-guide/basic-use.html");
                    return;
                }

                auto foundIt = s_registry->find(pClient);
                assert(foundIt != s_registry->end());
                if (foundIt != s_registry->end())
                {
                    s_registry->erase(foundIt);
                }
            }

            void TerminateAllComponents()
            {
                std::unique_lock<std::mutex> lock(s_registryMutex);

                if (!s_registry) {
                    // Registry already shut down -- nothing to do.
                    return;
                }

                for(const auto& it : *s_registry)
                {
                    if (it.second.TerminateFn) {
                        it.second.TerminateFn(it.first, -1);
                    }
                }
                s_registry->clear();
            }

        } // namespace ComponentRegistry
    } // namespace Utils
} // namespace Aws
