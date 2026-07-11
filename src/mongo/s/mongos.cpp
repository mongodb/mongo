// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/mongos_main.h"
#include "mongo/util/exit.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables mongoSMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[]) {
    mongo::exitCleanly(mongo::mongos_main(argc, mongo::WindowsCommandLine(argc, argvW).argv()));
}
#else
int main(int argc, char* argv[]) {
    mongo::exitCleanly(mongo::mongos_main(argc, argv));
}
#endif
