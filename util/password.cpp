
#include "../pch.h"
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

        cin >> password;

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

        cin >> password;

        if ( !SetConsoleMode( stdinh, old ) ) {
            cerr << "Cannot set console mode " << GetLastError() << "\n";
            return string();
        }
        cin.get();
#endif
        cout << "\n";
        return password;
    }
}
