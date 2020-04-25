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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/password.h"

#include <iostream>

#ifndef _WIN32
#include <termios.h>
#endif

#include "mongo/util/errno_util.h"
#include "mongo/util/password_params_gen.h"

namespace mongo {

std::string askPassword() {
    std::string password;
    std::cerr << "Enter password: ";

    if (newLineAfterPasswordPromptForTest) {
        std::cerr << "\n";
    }

#ifndef _WIN32
    const int stdinfd = 0;
    termios termio;
    tcflag_t old = 0;
    if (isatty(stdinfd)) {
        int i = tcgetattr(stdinfd, &termio);
        if (i == -1) {
            std::cerr << "Cannot get terminal attributes " << errnoWithDescription() << std::endl;
            return std::string();
        }
        old = termio.c_lflag;
        termio.c_lflag &= ~ECHO;
        i = tcsetattr(stdinfd, TCSANOW, &termio);
        if (i == -1) {
            std::cerr << "Cannot set terminal attributes " << errnoWithDescription() << std::endl;
            return std::string();
        }
    }

    getline(std::cin, password);

    if (isatty(stdinfd)) {
        termio.c_lflag = old;
        int i = tcsetattr(stdinfd, TCSANOW, &termio);
        if (i == -1) {
            std::cerr << "Cannot set terminal attributes " << errnoWithDescription() << std::endl;
            return std::string();
        }
    }
#else
    HANDLE stdinh = GetStdHandle(STD_INPUT_HANDLE);
    if (stdinh == INVALID_HANDLE_VALUE) {
        std::cerr << "Cannot get stdin handle " << GetLastError() << "\n";
        return std::string();
    }

    DWORD old;
    if (!GetConsoleMode(stdinh, &old)) {
        std::cerr << "Cannot get console mode " << GetLastError() << "\n";
        return std::string();
    }

    DWORD noecho = ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT;
    if (!SetConsoleMode(stdinh, noecho)) {
        std::cerr << "Cannot set console mode " << GetLastError() << "\n";
        return std::string();
    }

    getline(std::cin, password);

    if (!SetConsoleMode(stdinh, old)) {
        std::cerr << "Cannot set console mode " << GetLastError() << "\n";
        return std::string();
    }
#endif
    std::cerr << "\n";
    return password;
}
}  // namespace mongo
