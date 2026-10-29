// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/shell/mongo_main.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#ifdef _WIN32
int wmain(int argc, wchar_t* argvW[]) {
    mongo::quickExit(mongo::mongo_main(argc, mongo::WindowsCommandLine(argc, argvW).argv()));
}
#else   // #ifdef _WIN32
int main(int argc, char* argv[]) {
    mongo::quickExit(mongo::mongo_main(argc, argv));
}
#endif  // #ifdef _WIN32
