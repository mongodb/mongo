/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifdef __ANDROID__

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/memory/stl/AWSString.h>

#include <jni.h>

namespace Aws
{
namespace Platform
{

// must be called before any other native SDK functions when running on Android
AWS_CORE_API void InitAndroid(JNIEnv* env, jobject context);

// helper functions for functionality requiring JNI calls; not valid until InitAndroid has been called
AWS_CORE_API JavaVM* GetJavaVM();
AWS_CORE_API const char* GetCacheDirectory();

} //namespace Platform
} //namespace Aws

#endif // __ANDROID__
