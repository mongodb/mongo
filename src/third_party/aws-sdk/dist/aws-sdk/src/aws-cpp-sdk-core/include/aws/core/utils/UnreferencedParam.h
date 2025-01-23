/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

// taken from http://stackoverflow.com/questions/3020584/avoid-warning-unreferenced-formal-parameter, ugly but avoids having to #include the definition of an unreferenced struct/class

#if defined (_MSC_VER)

    #define AWS_UNREFERENCED_PARAM(x) (&reinterpret_cast<const int &>(x))

#else

    #define AWS_UNREFERENCED_PARAM(x) ((void)(x))

#endif // _MSC_VER
