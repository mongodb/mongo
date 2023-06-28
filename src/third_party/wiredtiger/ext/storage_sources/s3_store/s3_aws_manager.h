/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef S3AWSMANAGER
#define S3AWSMANAGER

#include <mutex>
#include <aws/core/Aws.h>

/*
 * The AWS SDK must only be initialized once and must call initialization and shutdown in the
 * correct order. The AwsManager handles multiple calls from the S3 extension initialization and
 * uses a reference counter to check how many instances of the S3 extension are using the SDK. The
 * first call to the extension will initiate the SDK while subsequent calls will increment the
 * reference counter. Then each call to terminate will decrement the reference counter until it
 * reaches 0 at which point SDK shutdown will be called.
 */
class AwsManager {
public:
    static AwsManager &
    Get()
    {
        return aws_instance;
    }

    // Public facing function to call the SDK initialization and abstract the reference to the
    // class.
    static void
    Init()
    {
        return Get().InitInternal();
    }

    // Public facing function to call the SDK shutdown and abstract the reference to the class.
    static void
    Terminate()
    {
        return Get().TerminateInternal();
    }

private:
    AwsManager()
    {
        refCount = 0;
    }

    // Check the number of references to the class and initialize the AWS SDK if it's the first
    // reference.
    void
    InitInternal()
    {
        std::lock_guard<std::mutex> lock(InitGuard);
        if (refCount == 0) {
            Aws::InitAPI(options);
        }
        refCount++;
    }

    // Check the number of references to the class and shutdown the AWS SDK if there are no more
    // references.
    void
    TerminateInternal()
    {
        std::lock_guard<std::mutex> lock(InitGuard);
        refCount--;
        if (refCount == 0) {
            Aws::ShutdownAPI(options);
        }
    }

    static AwsManager aws_instance;
    Aws::SDKOptions options;
    int refCount;
    static std::mutex InitGuard;
};
#endif
