/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/memory/AWSMemory.h>

#include <aws/core/utils/memory/MemorySystemInterface.h>
#include <aws/common/common.h>

#include <atomic>

using namespace Aws::Utils;
using namespace Aws::Utils::Memory;

#ifdef USE_AWS_MEMORY_MANAGEMENT
  static MemorySystemInterface* AWSMemorySystem(nullptr);
#endif // USE_AWS_MEMORY_MANAGEMENT

namespace Aws
{
namespace Utils
{
namespace Memory
{

void InitializeAWSMemorySystem(MemorySystemInterface& memorySystem)
{
    #ifdef USE_AWS_MEMORY_MANAGEMENT
        if(AWSMemorySystem != nullptr)
        {
            AWSMemorySystem->End();
        }

        AWSMemorySystem = &memorySystem;
        AWSMemorySystem->Begin();
    #else
        AWS_UNREFERENCED_PARAM(memorySystem);
    #endif // USE_AWS_MEMORY_MANAGEMENT
}

void ShutdownAWSMemorySystem(void)
{
    #ifdef USE_AWS_MEMORY_MANAGEMENT
        if(AWSMemorySystem != nullptr)
        {
            AWSMemorySystem->End();
        }
        AWSMemorySystem = nullptr;
    #endif // USE_AWS_MEMORY_MANAGEMENT
}

MemorySystemInterface* GetMemorySystem()
{
    #ifdef USE_AWS_MEMORY_MANAGEMENT
        return AWSMemorySystem;
    #else
        return nullptr;
    #endif // USE_AWS_MEMORY_MANAGEMENT
}

#if defined(__cpp_exceptions) || defined(_CPPUNWIND) || defined(__EXCEPTIONS)
#define AWS_HAS_EXCEPTIONS
#endif

/**
 * A default memory allocator
 * It is used in case of custom memory management SDK build
 *   and no custom allocator provided by application.
 */
class AwsDefaultMemorySystem : public MemorySystemInterface
{
public:
#if defined(AWS_HAS_EXCEPTIONS)
    static std::bad_alloc s_OOM;
#endif
    static AwsDefaultMemorySystem s_defMemSystem;

    virtual ~AwsDefaultMemorySystem() = default;

    void Begin() override
    {
    }

    void End() override
    {
    }

    void* AllocateMemory(std::size_t blockSize, std::size_t alignment, const char *allocationTag = nullptr) override
    {
        AWS_UNREFERENCED_PARAM(allocationTag);
        void *ret;

#if defined(AWS_HAS_ALIGNED_ALLOC)
        if (alignment == 1) {
            ret = malloc(blockSize);
        } else {
            ret = aligned_alloc(alignment, blockSize);
        }
#else
        AWS_UNREFERENCED_PARAM(alignment);
        ret = malloc(blockSize);
#endif
        if (ret == nullptr) {
#if defined(AWS_HAS_EXCEPTIONS)
            throw s_OOM;
#endif
        }
        return ret;
    }

    void FreeMemory(void* memoryPtr) override
    {
        free(memoryPtr);
    }
};
#if defined(AWS_HAS_EXCEPTIONS)
std::bad_alloc AwsDefaultMemorySystem::s_OOM;
#endif
AwsDefaultMemorySystem AwsDefaultMemorySystem::s_defMemSystem;

MemorySystemInterface& GetDefaultMemorySystem()
{
    return AwsDefaultMemorySystem::s_defMemSystem;
}

} // namespace Memory
} // namespace Utils

void* Malloc(const char* allocationTag, size_t allocationSize)
{
    Aws::Utils::Memory::MemorySystemInterface* memorySystem = Aws::Utils::Memory::GetMemorySystem();
#ifdef USE_AWS_MEMORY_MANAGEMENT
    // Was InitAPI forgotten or ShutdownAPI already called or Aws:: class used as static?
    // TODO: enforce to non-conditional assert
    AWS_ASSERT(memorySystem && "Memory system is not initialized.");
#endif

    void* rawMemory = nullptr;
    if(memorySystem != nullptr)
    {
        rawMemory = memorySystem->AllocateMemory(allocationSize, 1, allocationTag);
    }
    else
    {
        rawMemory = malloc(allocationSize);
    }

    return rawMemory;
}


void Free(void* memoryPtr)
{
    if(memoryPtr == nullptr)
    {
        return;
    }

    Aws::Utils::Memory::MemorySystemInterface* memorySystem = Aws::Utils::Memory::GetMemorySystem();
#ifdef USE_AWS_MEMORY_MANAGEMENT
    // Was InitAPI forgotten or ShutdownAPI already called or Aws:: class used as static?
    // TODO: enforce to non-conditional assert
    AWS_ASSERT(memorySystem && "Memory system is not initialized.");
#endif
    if(memorySystem != nullptr)
    {
        memorySystem->FreeMemory(memoryPtr);
    }
    else
    {
        free(memoryPtr);
    }
}

static void* MemAcquire(aws_allocator* allocator, size_t size)
{
    (void)allocator; // unused;
    return Aws::Malloc("CrtMemAcquire", size);
}

static void MemRelease(aws_allocator* allocator, void* ptr)
{
    (void)allocator; // unused;
    return Aws::Free(ptr);
}

static aws_allocator create_aws_allocator()
{
#if (__GNUC__ == 4) && !defined(__clang__)
    AWS_SUPPRESS_WARNING("-Wmissing-field-initializers", aws_allocator wrapper{};);
#else
    aws_allocator wrapper{};
#endif
    wrapper.mem_acquire = MemAcquire;
    wrapper.mem_release = MemRelease;
    wrapper.mem_realloc = nullptr;
    return wrapper;
}

aws_allocator* get_aws_allocator()
{
    static aws_allocator wrapper = create_aws_allocator();
    return &wrapper;
}

} // namespace Aws


