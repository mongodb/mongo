/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

// The following macros define the minimum required platform.  The minimum required platform
// is the earliest version of Windows, Internet Explorer etc. that has the necessary features to run
// your application.  The macros work by enabling all features available on platform versions up to
// and
// including the version specified.

// Modify the following defines if you have to target a platform prior to the ones specified below.
// Refer to MSDN for the latest info on corresponding values for different platforms.
#ifndef WINVER         // Specifies that the minimum required platform is Windows Vista.
#define WINVER 0x0600  // Change this to the appropriate value to target other versions of Windows.
#endif

#ifndef _WIN32_WINNT  // Specifies that the minimum required platform is Windows Vista.
#define _WIN32_WINNT \
    0x0600  // Change this to the appropriate value to target other versions of Windows.
#endif

#ifndef _WIN32_IE         // Specifies that the minimum required platform is Internet Explorer 6.0.
#define _WIN32_IE 0x0600  // Change this to the appropriate value to target other versions of IE.
#endif

#ifndef _WIN32_MSI      // Specifies that the minimum required MSI version is MSI 4.5
#define _WIN32_MSI 405  // Change this to the appropriate value to target other versions of MSI.
#endif
