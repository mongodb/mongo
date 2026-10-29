// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/password.h"

#include <iostream>

#ifndef _WIN32
#include <termios.h>
#endif

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/errno_util.h"
#include "mongo/util/password_params_gen.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


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
            auto ec = lastSystemError();
            std::cerr << "Cannot get terminal attributes " << errorMessage(ec) << std::endl;
            return std::string();
        }
        old = termio.c_lflag;
        termio.c_lflag &= ~ECHO;
        i = tcsetattr(stdinfd, TCSANOW, &termio);
        if (i == -1) {
            auto ec = lastSystemError();
            std::cerr << "Cannot set terminal attributes " << errorMessage(ec) << std::endl;
            return std::string();
        }
    }

    getline(std::cin, password);

    if (isatty(stdinfd)) {
        termio.c_lflag = old;
        int i = tcsetattr(stdinfd, TCSANOW, &termio);
        if (i == -1) {
            auto ec = lastSystemError();
            std::cerr << "Cannot set terminal attributes " << errorMessage(ec) << std::endl;
            return std::string();
        }
    }
#else
    HANDLE stdinh = GetStdHandle(STD_INPUT_HANDLE);
    if (stdinh == INVALID_HANDLE_VALUE) {
        auto ec = lastSystemError();
        std::cerr << "Cannot get stdin handle " << errorMessage(ec) << "\n";
        return std::string();
    }

    DWORD old;
    if (!GetConsoleMode(stdinh, &old)) {
        auto ec = lastSystemError();
        std::cerr << "Cannot get console mode " << errorMessage(ec) << "\n";
        return std::string();
    }

    DWORD noecho = ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT;
    if (!SetConsoleMode(stdinh, noecho)) {
        auto ec = lastSystemError();
        std::cerr << "Cannot set console mode " << errorMessage(ec) << "\n";
        return std::string();
    }

    getline(std::cin, password);

    if (!SetConsoleMode(stdinh, old)) {
        auto ec = lastSystemError();
        std::cerr << "Cannot set console mode " << errorMessage(ec) << "\n";
        return std::string();
    }
#endif
    std::cerr << "\n";
    return password;
}
}  // namespace mongo
