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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/password.h"

#include <iostream>

#ifndef _WIN32
#include <termios.h>
#endif

#include "mongo/util/log.h"
#include "mongo/util/password_params_gen.h"

namespace mongo {

#ifndef _WIN32
char getachar(void)  
{  
    struct termios tm, tm_temp;
    char chr;
    if (isatty(STDIN_FILENO)) {
        if(tcgetattr(STDIN_FILENO, &tm) < 0)
            return -1;
        tm_temp = tm;
        cfmakeraw(&tm);
        if(tcsetattr(STDIN_FILENO, TCSANOW, &tm) < 0)
            return -1;
    }
    chr = getchar();
    if (isatty(STDIN_FILENO)) {
        if(tcsetattr(STDIN_FILENO, TCSANOW, &tm_temp) < 0)
            return -1;
    }
    return chr;
}
#endif

std::string askPassword() {
    std::string password;
    std::cerr << "Enter password: ";

    if (newLineAfterPasswordPromptForTest) {
        std::cerr << "\n";
    }

#ifndef _WIN32
    char chr;
    while ((chr = getachar()) > 0) {
        if (chr == '\r' || chr == '\n')
            break;
        password.append(&chr);
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
