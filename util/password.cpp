/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "password.h"
#include <iostream>

#ifndef _WIN32
#include <termios.h>
#endif

using namespace std;

namespace mongo {

    string askPassword() {

        std::string password;
        cout << "Enter password: ";
#ifndef _WIN32
        const int stdinfd = 0;
        termios termio;
        tcflag_t old = 0;
        if ( isatty( stdinfd ) ) {
            int i = tcgetattr( stdinfd, &termio );
            if( i == -1 ) {
                cerr << "Cannot get terminal attributes " << errnoWithDescription() << endl;
                return string();
            }
            old = termio.c_lflag;
            termio.c_lflag &= ~ECHO;
            i = tcsetattr( stdinfd, TCSANOW, &termio );
            if( i == -1 ) {
                cerr << "Cannot set terminal attributes " << errnoWithDescription() << endl;
                return string();
            }
        }

        getline( cin, password );

        if ( isatty( stdinfd ) ) {
            termio.c_lflag = old;
            int i = tcsetattr( stdinfd, TCSANOW, &termio );
            if( i == -1 ) {
                cerr << "Cannot set terminal attributes " << errnoWithDescription() << endl;
                return string();
            }
        }
#else
        HANDLE stdinh = GetStdHandle( STD_INPUT_HANDLE );
        if ( stdinh == INVALID_HANDLE_VALUE) {
            cerr << "Cannot get stdin handle " << GetLastError() << "\n";
            return string();
        }

        DWORD old;
        if ( !GetConsoleMode( stdinh, &old ) ) {
            cerr << "Cannot get console mode " << GetLastError() << "\n";
            return string();
        }

        DWORD noecho = ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT;
        if ( !SetConsoleMode( stdinh, noecho ) ) {
            cerr << "Cannot set console mode " << GetLastError() << "\n";
            return string();
        }

        getline( cin, password );

        if ( !SetConsoleMode( stdinh, old ) ) {
            cerr << "Cannot set console mode " << GetLastError() << "\n";
            return string();
        }
#endif
        cout << "\n";
        return password;
    }
}
