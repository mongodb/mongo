/* linenoise.c -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 * 
 *   http://github.com/antirez/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * Copyright (c) 2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Switch to gets() if $TERM is something we can't support.
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - Completion?
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * CHA (Cursor Horizontal Absolute)
 *    Sequence: ESC [ n G
 *    Effect: moves cursor to column n (1 based)
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward of n chars
 *
 * The following are used to clear the screen: ESC [ H ESC [ 2 J
 * This is actually composed of two sequences:
 *
 * cursorhome
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED2 (Clear entire screen)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 * 
 */

#ifdef _WIN32

#include <conio.h>
#include <windows.h>
#include <io.h>
#define snprintf _snprintf  // Microsoft headers use underscores in some names
#define strcasecmp _stricmp
#define strdup _strdup
#define isatty _isatty
#define write _write
#define STDIN_FILENO 0

#else /* _WIN32 */

#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <cctype>
#include <wctype.h>

#endif /* _WIN32 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include "linenoise.h"
#include "linenoise_utf8.h"
#include "mk_wcwidth.h"
#include <string>
#include <vector>
#include <boost/smart_ptr/scoped_array.hpp>

using std::string;
using std::vector;

using boost::scoped_array;

using linenoise_utf8::UChar8;
using linenoise_utf8::UChar32;
using linenoise_utf8::copyString8to32;
using linenoise_utf8::copyString32;
using linenoise_utf8::copyString32to8;
using linenoise_utf8::strlen32;
using linenoise_utf8::strncmp32;
using linenoise_utf8::write32;
using linenoise_utf8::Utf8String;
using linenoise_utf8::Utf32String;

struct linenoiseCompletions {
    vector<Utf32String> completionStrings;
};

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096

// make control-characters more readable
#define ctrlChar( upperCaseASCII ) ( upperCaseASCII - 0x40 )

/**
 * Recompute widths of all characters in a UChar32 buffer
 * @param text          input buffer of Unicode characters
 * @param widths        output buffer of character widths
 * @param charCount     number of characters in buffer
 */
static void recomputeCharacterWidths( const UChar32* text, char* widths, int charCount ) {
    for ( int i = 0; i < charCount; ++i ) {
        widths[ i ] = mk_wcwidth( text[ i ] );
    }
}

/**
 * Calculate a new screen position given a starting position, screen width and character count
 * @param x             initial x position (zero-based)
 * @param y             initial y position (zero-based)
 * @param screenColumns screen column count
 * @param charCount     character positions to advance
 * @param xOut          returned x position (zero-based)
 * @param yOut          returned y position (zero-based)
 */
static void calculateScreenPosition( int x, int y, int screenColumns, int charCount, int& xOut, int& yOut ) {
    xOut = x;
    yOut = y;
    int charsRemaining = charCount;
    while ( charsRemaining > 0 ) {
        int charsThisRow = ( x + charsRemaining < screenColumns ) ? charsRemaining : screenColumns - x;
        xOut = x + charsThisRow;
        yOut = y;
        charsRemaining -= charsThisRow;
        x = 0;
        ++y;
    }
    if ( xOut == screenColumns ) {  // we have to special-case line wrap
        xOut = 0;
        ++yOut;
    }
}

static bool isControlChar( UChar32 testChar ) {
    return ( testChar < ' ' ) ||                        // C0 controls
           ( testChar >= 0x7F && testChar <= 0x9F );    // DEL and C1 controls
}

struct PromptBase {                     // a convenience struct for grouping prompt info
    Utf32String     promptText;             // our copy of the prompt text, edited
    char*           promptCharWidths;       // character widths from mk_wcwidth()
    int             promptChars;            // chars in promptText
    int             promptExtraLines;       // extra lines (beyond 1) occupied by prompt
    int             promptIndentation;      // column offset to end of prompt
    int             promptLastLinePosition; // index into promptText where last line begins
    int             promptPreviousInputLen; // promptChars of previous input line, for clearing
    int             promptCursorRowOffset;  // where the cursor is relative to the start of the prompt
    int             promptScreenColumns;    // width of screen in columns
    int             promptPreviousLen;      // help erasing
    int             promptErrorCode;        // error code (invalid UTF-8) or zero

    PromptBase() : promptPreviousInputLen( 0 ) { }
};

struct PromptInfo : public PromptBase {

    PromptInfo( const UChar8* textPtr, int columns ) {
        promptExtraLines = 0;
        promptLastLinePosition = 0;
        promptPreviousLen = 0;
        promptScreenColumns = columns;
        Utf32String tempUnicode( textPtr );

        // strip control characters from the prompt -- we do allow newline
        UChar32* pIn = tempUnicode.get();
        UChar32* pOut = pIn;
        while ( *pIn ) {
            UChar32 c = *pIn;
            if ( '\n' == c || !isControlChar( c ) ) {
                *pOut = c;
                ++pOut;
            }
            ++pIn;
        }
        *pOut = 0;
        promptChars = pOut - tempUnicode.get();
        promptText = tempUnicode;

        int x = 0;
        for ( int i = 0; i < promptChars; ++i ) {
            UChar32 c = promptText[i];
            if ( '\n' == c ) {
                x = 0;
                ++promptExtraLines;
                promptLastLinePosition = i + 1;
            }
            else {
                ++x;
                if ( x >= promptScreenColumns ) {
                    x = 0;
                    ++promptExtraLines;
                    promptLastLinePosition = i + 1;
                }
            }
        }
        promptIndentation = promptChars - promptLastLinePosition;
        promptCursorRowOffset = promptExtraLines;
    }
};

// Used with DynamicPrompt (history search)
//
static const Utf32String forwardSearchBasePrompt( reinterpret_cast<const UChar8*>( "(i-search)`" ) );
static const Utf32String reverseSearchBasePrompt( reinterpret_cast<const UChar8*>( "(reverse-i-search)`" ) );
static const Utf32String endSearchBasePrompt( reinterpret_cast<const UChar8*>( "': " ) );
static Utf32String previousSearchText;          // remembered across invocations of linenoise()

// changing prompt for "(reverse-i-search)`text':" etc.
//
struct DynamicPrompt : public PromptBase {
    Utf32String     searchText;                 // text we are searching for
    char*           searchCharWidths;           // character widths from mk_wcwidth()
    int             searchTextLen;              // chars in searchText
    int             direction;                  // current search direction, 1=forward, -1=reverse

    DynamicPrompt( PromptBase& pi, int initialDirection ) : searchTextLen( 0 ), direction( initialDirection ) {
        promptScreenColumns = pi.promptScreenColumns;
        promptCursorRowOffset = 0;
        Utf32String emptyString( 1 );
        searchText = emptyString;
        const Utf32String* basePrompt = ( direction > 0 ) ? &forwardSearchBasePrompt : &reverseSearchBasePrompt;
        size_t promptStartLength = basePrompt->length();
        promptChars = promptStartLength + endSearchBasePrompt.length();
        promptLastLinePosition = promptChars;   // TODO fix this, we are asssuming that the history prompt won't wrap (!)
        promptPreviousLen = promptChars;
        Utf32String tempUnicode( promptChars + 1 );
        memcpy( tempUnicode.get(), basePrompt->get(), sizeof( UChar32 ) * promptStartLength );
        memcpy( &tempUnicode[promptStartLength], endSearchBasePrompt.get(), sizeof( UChar32 ) * ( endSearchBasePrompt.length() + 1 ) );
        tempUnicode.initFromBuffer();
        promptText = tempUnicode;
        calculateScreenPosition( 0, 0, pi.promptScreenColumns, promptChars, promptIndentation, promptExtraLines );
    }

    void updateSearchPrompt( void ) {
        const Utf32String* basePrompt = ( direction > 0 ) ? &forwardSearchBasePrompt : &reverseSearchBasePrompt;
        size_t promptStartLength = basePrompt->length();
        promptChars = promptStartLength + searchTextLen + endSearchBasePrompt.length();
        Utf32String tempUnicode( promptChars + 1 );
        memcpy( tempUnicode.get(), basePrompt->get(), sizeof( UChar32 ) * promptStartLength );
        memcpy( &tempUnicode[promptStartLength], searchText.get(), sizeof( UChar32 ) * searchTextLen );
        size_t endIndex = promptStartLength + searchTextLen;
        memcpy( &tempUnicode[endIndex], endSearchBasePrompt.get(), sizeof( UChar32 ) * ( endSearchBasePrompt.length() + 1 ) );
        tempUnicode.initFromBuffer();
        promptText = tempUnicode;
    }

    void updateSearchText( const UChar32* textPtr ) {
        Utf32String tempUnicode( textPtr );
        searchTextLen = tempUnicode.chars();
        searchText = tempUnicode;
        updateSearchPrompt();
    }
};

class KillRing {
    static const int    capacity = 10;
    int                 size;
    int                 index;
    char                indexToSlot[10];
    vector<Utf32String> theRing;

public:
    enum                action { actionOther, actionKill, actionYank };
    action              lastAction;
    size_t              lastYankSize;

    KillRing() : size( 0 ), index( 0 ), lastAction( actionOther ) {
        theRing.reserve( capacity );
    }

    void kill( const UChar32* text, int textLen, bool forward ) {
        if ( textLen == 0 ) {
            return;
        }
        Utf32String killedText( text, textLen );
        if ( lastAction == actionKill && size > 0 ) {
            int slot = indexToSlot[0];
            int currentLen = theRing[slot].length();
            int resultLen = currentLen + textLen;
            Utf32String temp( resultLen + 1 );
            if ( forward ) {
                memcpy( temp.get(), theRing[slot].get(), currentLen * sizeof( UChar32 ) );
                memcpy( &temp[currentLen], killedText.get(), textLen * sizeof( UChar32 ) );
            }
            else {
                memcpy( temp.get(), killedText.get(), textLen * sizeof( UChar32 ) );
                memcpy( &temp[textLen], theRing[slot].get(), currentLen * sizeof( UChar32 ) );
            }
            temp[resultLen] = 0;
            temp.initFromBuffer();
            theRing[slot] = temp;
        }
        else {
            if ( size < capacity ) {
                if ( size > 0 ) {
                    memmove( &indexToSlot[1], &indexToSlot[0], size );
                }
                indexToSlot[0] = size;
                size++;
                theRing.push_back( killedText );
            }
            else {
                int slot = indexToSlot[capacity - 1];
                theRing[slot] = killedText;
                memmove( &indexToSlot[1], &indexToSlot[0], capacity - 1 );
                indexToSlot[0] = slot;
            }
            index = 0;
        }
    }

    Utf32String* yank() {
        return ( size > 0 ) ? &theRing[indexToSlot[index]] : 0;
    }

    Utf32String* yankPop() {
        if ( size == 0 ) {
            return 0;
        }
        ++index;
        if ( index == size ) {
            index = 0;
        }
        return &theRing[indexToSlot[index]];
    }

};

class InputBuffer {
    UChar32*    buf32;          // input buffer
    char*       charWidths;     // character widths from mk_wcwidth()
    int         buflen;         // buffer size in characters
    int         len;            // length of text in input buffer
    int         pos;            // character position in buffer ( 0 <= pos <= len )

    void clearScreen( PromptBase& pi );
    int incrementalHistorySearch( PromptBase& pi, int startChar );
    int completeLine( PromptBase& pi );
    void refreshLine( PromptBase& pi );

public:
    InputBuffer( UChar32* buffer, char* widthArray, int bufferLen ) : buf32( buffer ), charWidths( widthArray ), buflen( bufferLen - 1 ), len( 0 ), pos( 0 ) {
        buf32[0] = 0;
    }
    void preloadBuffer( const UChar8* preloadText ) {
        size_t ucharCount;
        int errorCode;
        copyString8to32( buf32, preloadText, buflen + 1, ucharCount, errorCode );
        recomputeCharacterWidths( buf32, charWidths, ucharCount );
        len = ucharCount;
        pos = ucharCount;
    }
    int getInputLine( PromptBase& pi );
    int length( void ) const { return len; }
};

// Special codes for keyboard input:
//
// Between Windows and the various Linux "terminal" programs, there is some
// pretty diverse behavior in the "scan codes" and escape sequences we are
// presented with.  So ... we'll translate them all into our own pidgin
// pseudocode, trying to stay out of the way of UTF-8 and international
// characters.  Here's the general plan.
//
// "User input keystrokes" (key chords, whatever) will be encoded as a single value.
// The low 21 bits are reserved for Unicode characters.  Popular function-type keys
// get their own codes in the range 0x10200000 to (if needed) 0x1FE00000, currently
// just arrow keys, Home, End and Delete.  Keypresses with Ctrl get ORed with
// 0x20000000, with Alt get ORed with 0x40000000.  So, Ctrl+Alt+Home is encoded
// as 0x20000000 + 0x40000000 + 0x10A00000 == 0x70A00000.  To keep things complicated,
// the Alt key is equivalent to prefixing the keystroke with ESC, so ESC followed by
// D is treated the same as Alt + D ... we'll just use Emacs terminology and call
// this "Meta".  So, we will encode both ESC followed by D and Alt held down while D
// is pressed the same, as Meta-D, encoded as 0x40000064.
//
// Here are the definitions of our component constants:
//
// Maximum unsigned 32-bit value    = 0xFFFFFFFF;   // For reference, max 32-bit value
// Highest allocated Unicode char   = 0x001FFFFF;   // For reference, max Unicode value
static const int META               = 0x40000000;   // Meta key combination
static const int CTRL               = 0x20000000;   // Ctrl key combination
static const int SPECIAL_KEY        = 0x10000000;   // Common bit for all special keys
static const int UP_ARROW_KEY       = 0x10200000;   // Special keys
static const int DOWN_ARROW_KEY     = 0x10400000;
static const int RIGHT_ARROW_KEY    = 0x10600000;
static const int LEFT_ARROW_KEY     = 0x10800000;
static const int HOME_KEY           = 0x10A00000;
static const int END_KEY            = 0x10C00000;
static const int DELETE_KEY         = 0x10E00000;
static const int PAGE_UP_KEY        = 0x11000000;
static const int PAGE_DOWN_KEY      = 0x11200000;

static const char* unsupported_term[] = { "dumb", "cons25", "emacs", NULL };
static linenoiseCompletionCallback* completionCallback = NULL;

#ifdef _WIN32
static HANDLE console_in, console_out;
static DWORD oldMode;
static WORD oldDisplayAttribute;
#else
static struct termios orig_termios; /* in order to restore at exit */
#endif

static KillRing killRing;

static int rawmode = 0; /* for atexit() function to check if restore is needed*/
static int atexit_registered = 0; /* register atexit just 1 time */
static int historyMaxLen = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int historyLen = 0;
static int historyIndex = 0;
static UChar8** history = NULL;

// used to emulate Windows command prompt on down-arrow after a recall
// we use -2 as our "not set" value because we add 1 to the previous index on down-arrow,
// and zero is a valid index (so -1 is a valid "previous index")
static int historyPreviousIndex = -2;
static bool historyRecallMostRecent = false;

static void linenoiseAtExit( void );

static bool isUnsupportedTerm( void ) {
    char* term = getenv( "TERM" );
    if ( term == NULL )
        return false;
    for ( int j = 0; unsupported_term[j]; ++j )
        if ( ! strcasecmp( term, unsupported_term[j] ) ) {
            return true;
        }
    return false;
}

static void beep() {
    fprintf( stderr, "\x7" );   // ctrl-G == bell/beep
    fflush( stderr );
}

void linenoiseHistoryFree( void ) {
    if ( history ) {
        for ( int j = 0; j < historyLen; ++j )
            free( history[j] );
        historyLen = 0;
        free( history );
        history = 0;
    }
}

static int enableRawMode( void ) {
#ifdef _WIN32
    if ( ! console_in ) {
        console_in = GetStdHandle( STD_INPUT_HANDLE );
        console_out = GetStdHandle( STD_OUTPUT_HANDLE );

        GetConsoleMode( console_in, &oldMode );
        SetConsoleMode( console_in, oldMode & ~( ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT ) );
    }
    return 0;
#else
    struct termios raw;

    if ( ! isatty( 0 ) ) goto fatal;
    if ( ! atexit_registered ) {
        atexit( linenoiseAtExit );
        atexit_registered = 1;
    }
    if ( tcgetattr( 0, &orig_termios ) == -1 ) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~( BRKINT | ICRNL | INPCK | ISTRIP | IXON );
    /* output modes - disable post processing */
    // this is wrong, we don't want raw output, it turns newlines into straight linefeeds
    //raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= ( CS8 );
    /* local modes - echoing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~( ECHO | ICANON | IEXTEN | ISIG );
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    /* put terminal in raw mode after flushing */
    if ( tcsetattr( 0, TCSADRAIN, &raw ) < 0 ) goto fatal;
    rawmode = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
#endif
}

static void disableRawMode( void ) {
#ifdef _WIN32
    SetConsoleMode( console_in, oldMode );
    console_in = 0;
    console_out = 0;
#else
    if ( rawmode && tcsetattr ( 0, TCSADRAIN, &orig_termios ) != -1 )
        rawmode = 0;
#endif
}

// At exit we'll try to fix the terminal to the initial conditions
static void linenoiseAtExit( void ) {
    disableRawMode();
}

static int getScreenColumns( void ) {
    int cols;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO inf;
    GetConsoleScreenBufferInfo( GetStdHandle( STD_OUTPUT_HANDLE ), &inf );
    cols = inf.dwSize.X;
#else
    struct winsize ws;
    cols = ( ioctl( 1, TIOCGWINSZ, &ws ) == -1 ) ? 80 : ws.ws_col;
#endif
    // cols is 0 in certain circumstances like inside debugger, which creates further issues
    return (cols > 0) ? cols : 80;
}

static int getScreenRows( void ) {
    int rows;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO inf;
    GetConsoleScreenBufferInfo( GetStdHandle( STD_OUTPUT_HANDLE ), &inf );
    rows = 1 + inf.srWindow.Bottom - inf.srWindow.Top;
#else
    struct winsize ws;
    rows = ( ioctl( 1, TIOCGWINSZ, &ws ) == -1 ) ? 24 : ws.ws_row;
#endif
    return (rows > 0) ? rows : 24;
}

static void setDisplayAttribute( bool enhancedDisplay ) {
#ifdef _WIN32
    if ( enhancedDisplay ) {
        CONSOLE_SCREEN_BUFFER_INFO inf;
        GetConsoleScreenBufferInfo( console_out, &inf );
        oldDisplayAttribute = inf.wAttributes;
        BYTE oldLowByte = oldDisplayAttribute & 0xFF;
        BYTE newLowByte;
        switch ( oldLowByte ) {
        case 0x07:
            //newLowByte = FOREGROUND_BLUE | FOREGROUND_INTENSITY;  // too dim
            //newLowByte = FOREGROUND_BLUE;                         // even dimmer
            newLowByte = FOREGROUND_BLUE | FOREGROUND_GREEN;        // most similar to xterm appearance
            break;
        case 0x70:
            newLowByte = BACKGROUND_BLUE | BACKGROUND_INTENSITY;
            break;
        default:
            newLowByte = oldLowByte ^ 0xFF;     // default to inverse video
            break;
        }
        inf.wAttributes = ( inf.wAttributes & 0xFF00 ) | newLowByte;
        SetConsoleTextAttribute( console_out, inf.wAttributes );
    }
    else {
        SetConsoleTextAttribute( console_out, oldDisplayAttribute );
    }
#else
    if ( enhancedDisplay ) {
        if ( write( 1, "\x1b[1;34m", 7 ) == -1 ) return; /* bright blue (visible with both B&W bg) */
    }
    else {
        if ( write( 1, "\x1b[0m", 4 ) == -1 ) return; /* reset */
    }
#endif
}

/**
 * Display the dynamic incremental search prompt and the current user input line.
 * @param pi   PromptBase struct holding information about the prompt and our screen position
 * @param buf32  input buffer to be displayed
 * @param len  count of characters in the buffer
 * @param pos  current cursor position within the buffer (0 <= pos <= len)
 */
static void dynamicRefresh( PromptBase& pi, UChar32* buf32, int len, int pos ) {

    // calculate the position of the end of the prompt
    int xEndOfPrompt, yEndOfPrompt;
    calculateScreenPosition( 0, 0, pi.promptScreenColumns, pi.promptChars, xEndOfPrompt, yEndOfPrompt );
    pi.promptIndentation = xEndOfPrompt;

    // calculate the position of the end of the input line
    int xEndOfInput, yEndOfInput;
    calculateScreenPosition( xEndOfPrompt, yEndOfPrompt, pi.promptScreenColumns, len, xEndOfInput, yEndOfInput );

    // calculate the desired position of the cursor
    int xCursorPos, yCursorPos;
    calculateScreenPosition( xEndOfPrompt, yEndOfPrompt, pi.promptScreenColumns, pos, xCursorPos, yCursorPos );

#ifdef _WIN32
    // position at the start of the prompt, clear to end of previous input
    CONSOLE_SCREEN_BUFFER_INFO inf;
    GetConsoleScreenBufferInfo( console_out, &inf );
    inf.dwCursorPosition.X = 0;
    inf.dwCursorPosition.Y -= pi.promptCursorRowOffset /*- pi.promptExtraLines*/;
    SetConsoleCursorPosition( console_out, inf.dwCursorPosition );
    DWORD count;
    FillConsoleOutputCharacterA( console_out, ' ', pi.promptPreviousLen + pi.promptPreviousInputLen, inf.dwCursorPosition, &count );
    pi.promptPreviousLen = pi.promptIndentation;
    pi.promptPreviousInputLen = len;

    // display the prompt
    if ( write32( 1, pi.promptText.get(), pi.promptChars ) == -1 ) return;

    // display the input line
    if ( write32( 1, buf32, len ) == -1 ) return;

    // position the cursor
    GetConsoleScreenBufferInfo( console_out, &inf );
    inf.dwCursorPosition.X = xCursorPos;  // 0-based on Win32
    inf.dwCursorPosition.Y -= yEndOfInput - yCursorPos;
    SetConsoleCursorPosition( console_out, inf.dwCursorPosition );
#else // _WIN32
    char seq[64];
    int cursorRowMovement = pi.promptCursorRowOffset - pi.promptExtraLines;
    if ( cursorRowMovement > 0 ) {  // move the cursor up as required
        snprintf( seq, sizeof seq, "\x1b[%dA", cursorRowMovement );
        if ( write( 1, seq, strlen( seq ) ) == -1 ) return;
    }
    // position at the start of the prompt, clear to end of screen
    snprintf( seq, sizeof seq, "\x1b[1G\x1b[J" );  // 1-based on VT100
    if ( write( 1, seq, strlen( seq ) ) == -1 ) return;

    // display the prompt
    if ( write32( 1, pi.promptText.get(), pi.promptChars ) == -1 ) return;

    // display the input line
    if ( write32( 1, buf32, len ) == -1 ) return;

    // we have to generate our own newline on line wrap
    if ( xEndOfInput == 0 && yEndOfInput > 0 )
        if ( write( 1, "\n", 1 ) == -1 ) return;

    // position the cursor
    cursorRowMovement = yEndOfInput - yCursorPos;
    if ( cursorRowMovement > 0 ) {  // move the cursor up as required
        snprintf( seq, sizeof seq, "\x1b[%dA", cursorRowMovement );
        if ( write( 1, seq, strlen( seq ) ) == -1 ) return;
    }
    // position the cursor within the line
    snprintf( seq, sizeof seq, "\x1b[%dG", xCursorPos + 1 );  // 1-based on VT100
    if ( write( 1, seq, strlen( seq ) ) == -1 ) return;
#endif

    pi.promptCursorRowOffset = pi.promptExtraLines + yCursorPos;  // remember row for next pass
}

/**
 * Refresh the user's input line: the prompt is already onscreen and is not redrawn here
 * @param pi   PromptBase struct holding information about the prompt and our screen position
 */
void InputBuffer::refreshLine( PromptBase& pi ) {

    // check for a matching brace/bracket/paren, remember its position if found
    int highlight = -1;
    if ( pos < len ) {
        /* this scans for a brace matching buf32[pos] to highlight */
        int scanDirection = 0;
        if ( strchr( "}])", buf32[pos] ) )
            scanDirection = -1; /* backwards */
        else if ( strchr( "{[(", buf32[pos] ) )
            scanDirection = 1; /* forwards */

        if ( scanDirection ) {
            int unmatched = scanDirection;
            for ( int i = pos + scanDirection; i >= 0 && i < len; i += scanDirection ) {
                /* TODO: the right thing when inside a string */
                if ( strchr( "}])", buf32[i] ) )
                    --unmatched;
                else if ( strchr( "{[(", buf32[i] ) )
                    ++unmatched;

                if ( unmatched == 0 ) {
                    highlight = i;
                    break;
                }
            }
        }
    }

    // calculate the position of the end of the input line
    int xEndOfInput, yEndOfInput;
    calculateScreenPosition( pi.promptIndentation, 0, pi.promptScreenColumns, len, xEndOfInput, yEndOfInput );

    // calculate the desired position of the cursor
    int xCursorPos, yCursorPos;
    calculateScreenPosition( pi.promptIndentation, 0, pi.promptScreenColumns, pos, xCursorPos, yCursorPos );

#ifdef _WIN32
    // position at the end of the prompt, clear to end of previous input
    CONSOLE_SCREEN_BUFFER_INFO inf;
    GetConsoleScreenBufferInfo( console_out, &inf );
    inf.dwCursorPosition.X = pi.promptIndentation;  // 0-based on Win32
    inf.dwCursorPosition.Y -= pi.promptCursorRowOffset - pi.promptExtraLines;
    SetConsoleCursorPosition( console_out, inf.dwCursorPosition );
    DWORD count;
    if ( len < pi.promptPreviousInputLen )
        FillConsoleOutputCharacterA( console_out, ' ', pi.promptPreviousInputLen, inf.dwCursorPosition, &count );
    pi.promptPreviousInputLen = len;

    // display the input line
    if (highlight == -1) {
        if ( write32( 1, buf32, len ) == -1 ) return;
    }
    else {
        if  (write32( 1, buf32, highlight ) == -1 ) return;
        setDisplayAttribute( true ); /* bright blue (visible with both B&W bg) */
        if ( write32( 1, &buf32[highlight], 1 ) == -1 ) return;
        setDisplayAttribute( false );
        if ( write32( 1, buf32 + highlight + 1, len - highlight - 1 ) == -1 ) return;
    }

    // position the cursor
    GetConsoleScreenBufferInfo( console_out, &inf );
    inf.dwCursorPosition.X = xCursorPos;  // 0-based on Win32
    inf.dwCursorPosition.Y -= yEndOfInput - yCursorPos;
    SetConsoleCursorPosition( console_out, inf.dwCursorPosition );
#else // _WIN32
    char seq[64];
    int cursorRowMovement = pi.promptCursorRowOffset - pi.promptExtraLines;
    if ( cursorRowMovement > 0 ) {  // move the cursor up as required
        snprintf( seq, sizeof seq, "\x1b[%dA", cursorRowMovement );
        if ( write( 1, seq, strlen( seq ) ) == -1 ) return;
    }
    // position at the end of the prompt, clear to end of screen
    snprintf( seq, sizeof seq, "\x1b[%dG\x1b[J", pi.promptIndentation + 1 );  // 1-based on VT100
    if ( write( 1, seq, strlen( seq ) ) == -1 ) return;

    if ( highlight == -1 ) {  // write unhighlighted text
        if ( write32( 1, buf32, len ) == -1 ) return;
    }
    else {  // highlight the matching brace/bracket/parenthesis
        if ( write32( 1, buf32, highlight ) == -1 ) return;
        setDisplayAttribute( true );
        if ( write32( 1, &buf32[highlight], 1 ) == -1 ) return;
        setDisplayAttribute( false );
        if ( write32( 1, buf32 + highlight + 1, len - highlight - 1 ) == -1 ) return;
    }

    // we have to generate our own newline on line wrap
    if ( xEndOfInput == 0 && yEndOfInput > 0 )
        if ( write( 1, "\n", 1 ) == -1 ) return;

    // position the cursor
    cursorRowMovement = yEndOfInput - yCursorPos;
    if ( cursorRowMovement > 0 ) {  // move the cursor up as required
        snprintf( seq, sizeof seq, "\x1b[%dA", cursorRowMovement );
        if ( write( 1, seq, strlen( seq ) ) == -1 ) return;
    }
    // position the cursor within the line
    snprintf( seq, sizeof seq, "\x1b[%dG", xCursorPos + 1 );  // 1-based on VT100
    if ( write( 1, seq, strlen( seq ) ) == -1 ) return;
#endif

    pi.promptCursorRowOffset = pi.promptExtraLines + yCursorPos;  // remember row for next pass
}

#ifndef _WIN32

/**
 * Read a UTF-8 sequence from the non-Windows keyboard and return the Unicode (UChar32) character it encodes
 *
 * @return  UChar32 Unicode character
 */
static UChar32 readUnicodeCharacter( void ) {
    static UChar8 utf8String[5];
    static size_t utf8Count = 0;
    while ( true ) {
        UChar8 c;
        if ( read( 0, &c, 1 ) <= 0 ) return 0;
        if ( c <= 0x7F ) {      // short circuit ASCII
            utf8Count = 0;
            return c;
        }
        else if ( utf8Count < sizeof( utf8String ) - 1 ) {
            utf8String[ utf8Count++ ] = c;
            utf8String[ utf8Count ] = 0;
            UChar32 unicodeChar[2];
            size_t ucharCount;
            int errorCode;
            copyString8to32( unicodeChar, utf8String, 2, ucharCount, errorCode );
            if ( ucharCount && errorCode == 0 ) {
                utf8Count = 0;
                return unicodeChar[0];
            }
        }
        else {
            utf8Count = 0;  // this shouldn't happen: got four bytes but no UTF-8 character
        }
    }
}

namespace EscapeSequenceProcessing { // move these out of global namespace

// This chunk of code does parsing of the escape sequences sent by various Linux terminals.
//
// It handles arrow keys, Home, End and Delete keys by interpreting the sequences sent by
// gnome terminal, xterm, rxvt, konsole, aterm and yakuake including the Alt and Ctrl key
// combinations that are understood by linenoise.
//
// The parsing uses tables, a bunch of intermediate dispatch routines and a doDispatch
// loop that reads the tables and sends control to "deeper" routines to continue the
// parsing.  The starting call to doDispatch( c, initialDispatch ) will eventually return
// either a character (with optional CTRL and META bits set), or -1 if parsing fails, or
// zero if an attempt to read from the keyboard fails.
//
// This is rather sloppy escape sequence processing, since we're not paying attention to what the
// actual TERM is set to and are processing all key sequences for all terminals, but it works with
// the most common keystrokes on the most common terminals.  It's intricate, but the nested 'if'
// statements required to do it directly would be worse.  This way has the advantage of allowing
// changes and extensions without having to touch a lot of code.

// This is a typedef for the routine called by doDispatch().  It takes the current character
// as input, does any required processing including reading more characters and calling other
// dispatch routines, then eventually returns the final (possibly extended or special) character.
//
typedef UChar32 ( *CharacterDispatchRoutine )( UChar32 );

// This structure is used by doDispatch() to hold a list of characters to test for and
// a list of routines to call if the character matches.  The dispatch routine list is one
// longer than the character list; the final entry is used if no character matches.
//
struct CharacterDispatch {
    unsigned int                len;        // length of the chars list
    const char*                 chars;      // chars to test
    CharacterDispatchRoutine*   dispatch;   // array of routines to call
};

// This dispatch routine is given a dispatch table and then farms work out to routines
// listed in the table based on the character it is called with.  The dispatch routines can
// read more input characters to decide what should eventually be returned.  Eventually,
// a called routine returns either a character or -1 to indicate parsing failure.
//
static UChar32 doDispatch( UChar32 c, CharacterDispatch& dispatchTable ) {
    for ( unsigned int i = 0; i < dispatchTable.len ; ++i ) {
        if ( static_cast<unsigned char>( dispatchTable.chars[i] ) == c ) {
            return dispatchTable.dispatch[i]( c );
        }
    }
    return dispatchTable.dispatch[dispatchTable.len]( c );
}

static UChar32 thisKeyMetaCtrl = 0;     // holds pre-set Meta and/or Ctrl modifiers

// Final dispatch routines -- return something
//
static UChar32 normalKeyRoutine( UChar32 c )            { return thisKeyMetaCtrl | c; }
static UChar32 upArrowKeyRoutine( UChar32 c )           { return thisKeyMetaCtrl | UP_ARROW_KEY; }
static UChar32 downArrowKeyRoutine( UChar32 c )         { return thisKeyMetaCtrl | DOWN_ARROW_KEY; }
static UChar32 rightArrowKeyRoutine( UChar32 c )        { return thisKeyMetaCtrl | RIGHT_ARROW_KEY; }
static UChar32 leftArrowKeyRoutine( UChar32 c )         { return thisKeyMetaCtrl | LEFT_ARROW_KEY; }
static UChar32 homeKeyRoutine( UChar32 c )              { return thisKeyMetaCtrl | HOME_KEY; }
static UChar32 endKeyRoutine( UChar32 c )               { return thisKeyMetaCtrl | END_KEY; }
static UChar32 pageUpKeyRoutine( UChar32 c )            { return thisKeyMetaCtrl | PAGE_UP_KEY; }
static UChar32 pageDownKeyRoutine( UChar32 c )          { return thisKeyMetaCtrl | PAGE_DOWN_KEY; }
static UChar32 deleteCharRoutine( UChar32 c )           { return thisKeyMetaCtrl | ctrlChar( 'H' ); } // key labeled Backspace
static UChar32 deleteKeyRoutine( UChar32 c )            { return thisKeyMetaCtrl | DELETE_KEY; }      // key labeled Delete
static UChar32 ctrlUpArrowKeyRoutine( UChar32 c )       { return thisKeyMetaCtrl | CTRL | UP_ARROW_KEY; }
static UChar32 ctrlDownArrowKeyRoutine( UChar32 c )     { return thisKeyMetaCtrl | CTRL | DOWN_ARROW_KEY; }
static UChar32 ctrlRightArrowKeyRoutine( UChar32 c )    { return thisKeyMetaCtrl | CTRL | RIGHT_ARROW_KEY; }
static UChar32 ctrlLeftArrowKeyRoutine( UChar32 c )     { return thisKeyMetaCtrl | CTRL | LEFT_ARROW_KEY; }
static UChar32 escFailureRoutine( UChar32 c )           { beep(); return -1; }

// Handle ESC [ 1 ; 3 (or 5) <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket1Semicolon3or5Routines[] = {
    upArrowKeyRoutine,
    downArrowKeyRoutine,
    rightArrowKeyRoutine,
    leftArrowKeyRoutine,
    escFailureRoutine
};
static CharacterDispatch escLeftBracket1Semicolon3or5Dispatch = { 4, "ABCD", escLeftBracket1Semicolon3or5Routines };

// Handle ESC [ 1 ; <more stuff> escape sequences
//
static UChar32 escLeftBracket1Semicolon3Routine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    thisKeyMetaCtrl |= META;
    return doDispatch( c, escLeftBracket1Semicolon3or5Dispatch );
}
static UChar32 escLeftBracket1Semicolon5Routine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    thisKeyMetaCtrl |= CTRL;
    return doDispatch( c, escLeftBracket1Semicolon3or5Dispatch );
}
static CharacterDispatchRoutine escLeftBracket1SemicolonRoutines[] = {
    escLeftBracket1Semicolon3Routine,
    escLeftBracket1Semicolon5Routine,
    escFailureRoutine
};
static CharacterDispatch escLeftBracket1SemicolonDispatch = { 2, "35", escLeftBracket1SemicolonRoutines };

// Handle ESC [ 1 <more stuff> escape sequences
//
static UChar32 escLeftBracket1SemicolonRoutine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escLeftBracket1SemicolonDispatch );
}
static CharacterDispatchRoutine escLeftBracket1Routines[] = {
    homeKeyRoutine,
    escLeftBracket1SemicolonRoutine,
    escFailureRoutine
};
static CharacterDispatch escLeftBracket1Dispatch = { 2, "~;", escLeftBracket1Routines };

// Handle ESC [ 3 <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket3Routines[] = {
    deleteKeyRoutine,
    escFailureRoutine
};
static CharacterDispatch escLeftBracket3Dispatch = { 1, "~", escLeftBracket3Routines };

// Handle ESC [ 4 <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket4Routines[] = {
    endKeyRoutine,
    escFailureRoutine
};
static CharacterDispatch escLeftBracket4Dispatch = { 1, "~", escLeftBracket4Routines };

// Handle ESC [ 5 <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket5Routines[] = {
    pageUpKeyRoutine,
    escFailureRoutine
};
static CharacterDispatch escLeftBracket5Dispatch = { 1, "~", escLeftBracket5Routines };

// Handle ESC [ 6 <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket6Routines[] = {
    pageDownKeyRoutine,
    escFailureRoutine
};
static CharacterDispatch escLeftBracket6Dispatch = { 1, "~", escLeftBracket6Routines };

// Handle ESC [ 7 <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket7Routines[] = {
    homeKeyRoutine,
    escFailureRoutine
};
static CharacterDispatch escLeftBracket7Dispatch = { 1, "~", escLeftBracket7Routines };

// Handle ESC [ 8 <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket8Routines[] = {
    endKeyRoutine,
    escFailureRoutine
};
static CharacterDispatch escLeftBracket8Dispatch = { 1, "~", escLeftBracket8Routines };

// Handle ESC [ <digit> escape sequences
//
static UChar32 escLeftBracket0Routine( UChar32 c ) {
    return escFailureRoutine( c );
}
static UChar32 escLeftBracket1Routine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escLeftBracket1Dispatch );
}
static UChar32 escLeftBracket2Routine( UChar32 c ) {
    return escFailureRoutine( c );  // Insert key, unused
}
static UChar32 escLeftBracket3Routine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escLeftBracket3Dispatch );
}
static UChar32 escLeftBracket4Routine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escLeftBracket4Dispatch );
}
static UChar32 escLeftBracket5Routine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escLeftBracket5Dispatch );
}
static UChar32 escLeftBracket6Routine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escLeftBracket6Dispatch );
}
static UChar32 escLeftBracket7Routine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escLeftBracket7Dispatch );
}
static UChar32 escLeftBracket8Routine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escLeftBracket8Dispatch );
}
static UChar32 escLeftBracket9Routine( UChar32 c ) {
    return escFailureRoutine( c );
}

// Handle ESC [ <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracketRoutines[] = {
    upArrowKeyRoutine,
    downArrowKeyRoutine,
    rightArrowKeyRoutine,
    leftArrowKeyRoutine,
    homeKeyRoutine,
    endKeyRoutine,
    escLeftBracket0Routine,
    escLeftBracket1Routine,
    escLeftBracket2Routine,
    escLeftBracket3Routine,
    escLeftBracket4Routine,
    escLeftBracket5Routine,
    escLeftBracket6Routine,
    escLeftBracket7Routine,
    escLeftBracket8Routine,
    escLeftBracket9Routine,
    escFailureRoutine
};
static CharacterDispatch escLeftBracketDispatch = { 16, "ABCDHF0123456789", escLeftBracketRoutines };

// Handle ESC O <char> escape sequences
//
static CharacterDispatchRoutine escORoutines[] = {
    upArrowKeyRoutine,
    downArrowKeyRoutine,
    rightArrowKeyRoutine,
    leftArrowKeyRoutine,
    homeKeyRoutine,
    endKeyRoutine,
    ctrlUpArrowKeyRoutine,
    ctrlDownArrowKeyRoutine,
    ctrlRightArrowKeyRoutine,
    ctrlLeftArrowKeyRoutine,
    escFailureRoutine
};
static CharacterDispatch escODispatch = { 10, "ABCDHFabcd", escORoutines };

// Initial ESC dispatch -- could be a Meta prefix or the start of an escape sequence
//
static UChar32 escLeftBracketRoutine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escLeftBracketDispatch );
}
static UChar32 escORoutine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escODispatch );
}
static UChar32 setMetaRoutine( UChar32 c ); // need forward reference
static CharacterDispatchRoutine escRoutines[] = {
    escLeftBracketRoutine,
    escORoutine,
    setMetaRoutine
};
static CharacterDispatch escDispatch = { 2, "[O", escRoutines };

// Initial dispatch -- we are not in the middle of anything yet
//
static UChar32 escRoutine( UChar32 c ) {
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;
    return doDispatch( c, escDispatch );
}
static CharacterDispatchRoutine initialRoutines[] = {
    escRoutine,
    deleteCharRoutine,
    normalKeyRoutine
};
static CharacterDispatch initialDispatch = { 2, "\x1B\x7F", initialRoutines };

// Special handling for the ESC key because it does double duty
//
static UChar32 setMetaRoutine( UChar32 c ) {
    thisKeyMetaCtrl = META;
    if ( c == 0x1B ) {  // another ESC, stay in ESC processing mode
        c = readUnicodeCharacter();
        if ( c == 0 ) return 0;
        return doDispatch( c, escDispatch );
    }
    return doDispatch( c, initialDispatch );
}

} // namespace EscapeSequenceProcessing // move these out of global namespace

#endif // #ifndef _WIN32

// linenoiseReadChar -- read a keystroke or keychord from the keyboard, and translate it
// into an encoded "keystroke".  When convenient, extended keys are translated into their
// simpler Emacs keystrokes, so an unmodified "left arrow" becomes Ctrl-B.
//
// A return value of zero means "no input available", and a return value of -1 means "invalid key".
//
static UChar32 linenoiseReadChar( void ) {
#ifdef _WIN32

    INPUT_RECORD rec;
    DWORD count;
    int modifierKeys = 0;
    bool escSeen = false;
    while ( true ) {
        ReadConsoleInputW( console_in, &rec, 1, &count );
#if 0 // helper for debugging keystrokes, display info in the debug "Output" window in the debugger
        {
            if ( rec.EventType == KEY_EVENT ) {
                //if ( rec.Event.KeyEvent.uChar.UnicodeChar ) {
                    char buf[1024];
                    sprintf(
                            buf,
                            "Unicode character 0x%04X, repeat count %d, virtual keycode 0x%04X, virtual scancode 0x%04X, key %s%s%s%s%s\n",
                            rec.Event.KeyEvent.uChar.UnicodeChar,
                            rec.Event.KeyEvent.wRepeatCount,
                            rec.Event.KeyEvent.wVirtualKeyCode,
                            rec.Event.KeyEvent.wVirtualScanCode,
                            rec.Event.KeyEvent.bKeyDown ? "down" : "up",
                                (rec.Event.KeyEvent.dwControlKeyState & LEFT_CTRL_PRESSED)  ? " L-Ctrl" : "",
                                (rec.Event.KeyEvent.dwControlKeyState & RIGHT_CTRL_PRESSED) ? " R-Ctrl" : "",
                                (rec.Event.KeyEvent.dwControlKeyState & LEFT_ALT_PRESSED)   ? " L-Alt"  : "",
                                (rec.Event.KeyEvent.dwControlKeyState & RIGHT_ALT_PRESSED)  ? " R-Alt"  : ""
                    );
                    OutputDebugStringA( buf );
                //}
            }
        }
#endif
        if ( rec.EventType != KEY_EVENT ) {
            continue;
        }
        // Windows provides for entry of characters that are not on your keyboard by sending the Unicode
        // characters as a "key up" with virtual keycode 0x12 (VK_MENU == Alt key) ... accept these characters,
        // otherwise only process characters on "key down"
        if ( !rec.Event.KeyEvent.bKeyDown && rec.Event.KeyEvent.wVirtualKeyCode != VK_MENU ) {
            continue;
        }
        modifierKeys = 0;
        // AltGr is encoded as ( LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED ), so don't treat this combination as either CTRL or META
        // we just turn off those two bits, so it is still possible to combine CTRL and/or META with an AltGr key by using right-Ctrl and/or left-Alt
        if ( ( rec.Event.KeyEvent.dwControlKeyState & ( LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED ) ) == ( LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED ) ) {
            rec.Event.KeyEvent.dwControlKeyState &= ~( LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED );
        }
        if ( rec.Event.KeyEvent.dwControlKeyState & ( RIGHT_CTRL_PRESSED | LEFT_CTRL_PRESSED ) ) {
            modifierKeys |= CTRL;
        }
        if ( rec.Event.KeyEvent.dwControlKeyState & ( RIGHT_ALT_PRESSED | LEFT_ALT_PRESSED ) ) {
            modifierKeys |= META;
        }
        if ( escSeen ) {
            modifierKeys |= META;
        }
        if ( rec.Event.KeyEvent.uChar.UnicodeChar == 0 ) {
            switch ( rec.Event.KeyEvent.wVirtualKeyCode ) {
                case VK_LEFT:   return modifierKeys | LEFT_ARROW_KEY;
                case VK_RIGHT:  return modifierKeys | RIGHT_ARROW_KEY;
                case VK_UP:     return modifierKeys | UP_ARROW_KEY;
                case VK_DOWN:   return modifierKeys | DOWN_ARROW_KEY;
                case VK_DELETE: return modifierKeys | DELETE_KEY;
                case VK_HOME:   return modifierKeys | HOME_KEY;
                case VK_END:    return modifierKeys | END_KEY;
                case VK_PRIOR:  return modifierKeys | PAGE_UP_KEY;
                case VK_NEXT:   return modifierKeys | PAGE_DOWN_KEY;
                default: continue;                      // in raw mode, ReadConsoleInput shows shift, ctrl ...
            }                                           //  ... ignore them
        }
        else if ( rec.Event.KeyEvent.uChar.UnicodeChar == ctrlChar( '[' ) ) { // ESC, set flag for later
            escSeen = true;
            continue;
        }
        else {
            // we got a real character, return it
            return modifierKeys | rec.Event.KeyEvent.uChar.UnicodeChar;
        }
    }

#else
    UChar32 c;
    c = readUnicodeCharacter();
    if ( c == 0 ) return 0;

    // If _DEBUG_LINUX_KEYBOARD is set, then ctrl-^ puts us into a keyboard debugging mode
    // where we print out decimal and decoded values for whatever the "terminal" program
    // gives us on different keystrokes.  Hit ctrl-C to exit this mode.
    //
#define _DEBUG_LINUX_KEYBOARD
#if defined(_DEBUG_LINUX_KEYBOARD)
    if ( c == ctrlChar( '^' ) ) {    // ctrl-^, special debug mode, prints all keys hit, ctrl-C to get out
        printf( "\nEntering keyboard debugging mode (on ctrl-^), press ctrl-C to exit this mode\n" );
        while ( true ) {
            unsigned char keys[10];
            int ret = read( 0, keys, 10 );

            if ( ret <= 0 ) {
                printf( "\nret: %d\n", ret );
            }
            for ( int i = 0; i < ret; ++i ) {
                UChar32 key = static_cast<UChar32>( keys[i] );
                char* friendlyTextPtr;
                char friendlyTextBuf[10];
                const char* prefixText = (key < 0x80) ? "" : "0x80+";
                UChar32 keyCopy = (key < 0x80) ? key : key - 0x80;
                if ( keyCopy >= '!' && keyCopy <= '~' ) {   // printable
                    friendlyTextBuf[0] = '\'';
                    friendlyTextBuf[1] = keyCopy;
                    friendlyTextBuf[2] = '\'';
                    friendlyTextBuf[3] = 0;
                    friendlyTextPtr = friendlyTextBuf;
                }
                else if ( keyCopy == ' ' ) {
                    friendlyTextPtr = const_cast<char*>( "space" );
                }
                else if (keyCopy == 27 ) {
                    friendlyTextPtr = const_cast<char*>( "ESC" );
                }
                else if (keyCopy == 0 ) {
                    friendlyTextPtr = const_cast<char*>( "NUL" );
                }
                else if (keyCopy == 127 ) {
                    friendlyTextPtr = const_cast<char*>( "DEL" );
                }
                else {
                    friendlyTextBuf[0] = '^';
                    friendlyTextBuf[1] = keyCopy + 0x40;
                    friendlyTextBuf[2] = 0;
                    friendlyTextPtr = friendlyTextBuf;
                }
                printf( "%d x%02X (%s%s)  ", key,  key, prefixText, friendlyTextPtr );
            }
            printf( "\x1b[1G\n" );  // go to first column of new line

            // drop out of this loop on ctrl-C
            if ( keys[0] == ctrlChar( 'C' ) ) {
                printf( "Leaving keyboard debugging mode (on ctrl-C)\n" );
                fflush( stdout );
                return -2;
            }
        }
    }
#endif  // _DEBUG_LINUX_KEYBOARD

    EscapeSequenceProcessing::thisKeyMetaCtrl = 0;    // no modifiers yet at initialDispatch
    return EscapeSequenceProcessing::doDispatch( c, EscapeSequenceProcessing::initialDispatch );
#endif // #_WIN32
}

/**
 * Free memory used in a recent command completion session
 *
 * @param lc pointer to a linenoiseCompletions struct
 */
static void freeCompletions( linenoiseCompletions* lc ) {
    lc->completionStrings.clear();
}

/**
 * convert {CTRL + 'A'}, {CTRL + 'a'} and {CTRL + ctrlChar( 'A' )} into ctrlChar( 'A' )
 * leave META alone
 *
 * @param c character to clean up
 * @return cleaned-up character
 */
static int cleanupCtrl( int c ) {
    if ( c & CTRL ) {
        int d = c & 0x1FF;
        if ( d >= 'a' && d <= 'z' ) {
            c = ( c + ( 'a' - ctrlChar( 'A' ) ) ) & ~CTRL;
        }
        if ( d >= 'A' && d <= 'Z' ) {
            c = ( c + ( 'A' - ctrlChar( 'A' ) ) ) & ~CTRL;
        }
        if ( d >= ctrlChar( 'A' ) && d <= ctrlChar( 'Z' ) ) {
            c = c & ~CTRL;
        }
    }
    return c;
}

// break characters that may precede items to be completed
static const char breakChars[] = " =+-/\\*?\"'`&<>;|@{([])}";

// maximum number of completions to display without asking
static const size_t completionCountCutoff = 100;

/**
 * Handle command completion, using a completionCallback() routine to provide possible substitutions
 * This routine handles the mechanics of updating the user's input buffer with possible replacement of
 * text as the user selects a proposed completion string, or cancels the completion attempt.
 * @param pi     PromptBase struct holding information about the prompt and our screen position
 */
int InputBuffer::completeLine( PromptBase& pi ) {
    linenoiseCompletions lc;
    char c = 0;

    // completionCallback() expects a parsable entity, so find the previous break character and extract
    // a copy to parse.  we also handle the case where tab is hit while not at end-of-line.
    int startIndex = pos;
    while ( --startIndex >= 0 ) {
        if ( strchr( breakChars, buf32[startIndex] ) ) {
            break;
        }
    }
    ++startIndex;
    int itemLength = pos - startIndex;
    Utf32String unicodeCopy( &buf32[startIndex], itemLength );
    Utf8String parseItem( unicodeCopy );

    // get a list of completions
    completionCallback( reinterpret_cast< char * >( parseItem.get() ), &lc );

    // if no completions, we are done
    if ( lc.completionStrings.size() == 0 ) {
        beep();
        freeCompletions( &lc );
        return 0;
    }

    // at least one completion
    int longestCommonPrefix = 0;
    int displayLength = 0;
    if ( lc.completionStrings.size() == 1 ) {
        longestCommonPrefix = lc.completionStrings[0].length();
    }
    else {
        bool keepGoing = true;
        while ( keepGoing ) {
            for ( size_t j = 0; j < lc.completionStrings.size() - 1; ++j ) {
                char c1 = lc.completionStrings[j][longestCommonPrefix];
                char c2 = lc.completionStrings[j + 1][longestCommonPrefix];
                if ( ( 0 == c1 ) || ( 0 == c2 ) || ( c1 != c2 ) ) {
                    keepGoing = false;
                    break;
                }
            }
            if ( keepGoing ) {
                ++longestCommonPrefix;
            }
        }
    }
    if ( lc.completionStrings.size() != 1 ) {    // beep if ambiguous
        beep();
    }

    // if we can extend the item, extend it and return to main loop
    if ( longestCommonPrefix > itemLength ) {
        displayLength = len + longestCommonPrefix - itemLength;
        if ( displayLength > buflen ) {
            longestCommonPrefix -= displayLength - buflen;  // don't overflow buffer
            displayLength = buflen;             // truncate the insertion
            beep();                             // and make a noise
        }
        Utf32String displayText( displayLength + 1 );
        memcpy( displayText.get(), buf32, sizeof( UChar32 ) * startIndex );
        memcpy( &displayText[startIndex], &lc.completionStrings[0][0], sizeof( UChar32 ) * longestCommonPrefix );
        int tailIndex = startIndex + longestCommonPrefix;
        memcpy( &displayText[tailIndex], &buf32[pos], sizeof( UChar32 ) * ( displayLength - tailIndex + 1 ) );
        copyString32( buf32, displayText.get(), buflen + 1 );
        pos = startIndex + longestCommonPrefix;
        len = displayLength;
        refreshLine( pi );
        return 0;
    }

    // we can't complete any further, wait for second tab
    do {
        c = linenoiseReadChar();
        c = cleanupCtrl( c );
    } while ( c == static_cast<char>( -1 ) );

    // if any character other than tab, pass it to the main loop
    if ( c != ctrlChar( 'I' ) ) {
        freeCompletions( &lc );
        return c;
    }

    // we got a second tab, maybe show list of possible completions
    bool showCompletions = true;
    bool onNewLine = false;
    if ( lc.completionStrings.size() > completionCountCutoff ) {
        int savePos = pos;  // move cursor to EOL to avoid overwriting the command line
        pos = len;
        refreshLine( pi );
        pos = savePos;
        printf( "\nDisplay all %u possibilities? (y or n)",
                static_cast<unsigned int>( lc.completionStrings.size() ) );
        fflush( stdout );
        onNewLine = true;
        while ( c != 'y' && c != 'Y' && c != 'n' && c != 'N' && c != ctrlChar( 'C' ) ) {
            do {
                c = linenoiseReadChar();
                c = cleanupCtrl( c );
            } while ( c == static_cast<char>( -1 ) );
        }
        switch ( c ) {
        case 'n':
        case 'N':
            showCompletions = false;
            freeCompletions( &lc );
            break;
        case ctrlChar( 'C' ):
            showCompletions = false;
            freeCompletions( &lc );
            if ( write( 1, "^C", 2 ) == -1 ) return -1;    // Display the ^C we got
            c = 0;
            break;
        }
    }

    // if showing the list, do it the way readline does it
    bool stopList = false;
    if ( showCompletions ) {
        int longestCompletion = 0;
        for ( size_t j = 0; j < lc.completionStrings.size(); ++j) {
            itemLength = lc.completionStrings[j].length();
            if ( itemLength > longestCompletion ) {
                longestCompletion = itemLength;
            }
        }
        longestCompletion += 2;
        int columnCount = pi.promptScreenColumns / longestCompletion;
        if ( columnCount < 1) {
            columnCount = 1;
        }
        if ( ! onNewLine ) {    // skip this if we showed "Display all %d possibilities?"
            int savePos = pos;  // move cursor to EOL to avoid overwriting the command line
            pos = len;
            refreshLine( pi );
            pos = savePos;
        }
        size_t pauseRow = getScreenRows() - 1;
        size_t rowCount = ( lc.completionStrings.size() + columnCount - 1) / columnCount;
        for ( size_t row = 0; row < rowCount; ++row ) {
            if ( row == pauseRow ) {
                printf( "\n--More--" );
                fflush( stdout );
                c = 0;
                bool doBeep = false;
                while ( c != ' ' && c != '\r' && c != '\n' && c != 'y' && c != 'Y' && c != 'n' && c != 'N' && c != 'q' && c != 'Q' && c != ctrlChar( 'C' ) ) {
                    if ( doBeep ) {
                        beep();
                    }
                    doBeep = true;
                    do {
                        c = linenoiseReadChar();
                        c = cleanupCtrl( c );
                    } while ( c == static_cast<char>( -1 ) );
                }
                switch ( c ) {
                case ' ':
                case 'y':
                case 'Y':
                    printf( "\r        \r" );
                    pauseRow += getScreenRows() - 1;
                    break;
                case '\r':
                case '\n':
                    printf( "\r        \r" );
                    ++pauseRow;
                    break;
                case 'n':
                case 'N':
                case 'q':
                case 'Q':
                    printf( "\r        \r" );
                    stopList = true;
                    break;
                case ctrlChar( 'C' ):
                    if ( write( 1, "^C", 2 ) == -1 ) return -1;    // Display the ^C we got
                    stopList = true;
                    break;
                }
            }
            else {
                printf( "\n" );
            }
            if ( stopList ) {
                break;
            }
            for ( int column = 0; column < columnCount; ++column ) {
                size_t index = ( column * rowCount ) + row;
                if ( index < lc.completionStrings.size() ) {
                    itemLength = lc.completionStrings[index].length();
                    fflush( stdout );
                    if ( write32( 1, lc.completionStrings[index].get(), itemLength ) == -1 ) return -1;
                    if ( ( ( column + 1 ) * rowCount ) + row < lc.completionStrings.size() ) {
                        for ( int k = itemLength; k < longestCompletion; ++k ) {
                            printf( " " );
                        }
                    }
                }
            }
        }
        fflush( stdout );
        freeCompletions( &lc );
    }

    // display the prompt on a new line, then redisplay the input buffer
    if ( ! stopList || c == ctrlChar( 'C' ) ) {
        if ( write( 1, "\n", 1 ) == -1 ) return 0;
    }
    if ( write32( 1, pi.promptText.get(), pi.promptChars ) == -1 ) return 0;
#ifndef _WIN32
    // we have to generate our own newline on line wrap on Linux
    if ( pi.promptIndentation == 0 && pi.promptExtraLines > 0 )
        if ( write( 1, "\n", 1 ) == -1 ) return 0;
#endif
    pi.promptCursorRowOffset = pi.promptExtraLines;
    refreshLine( pi );
    return 0;
}

/**
 * Clear the screen ONLY (no redisplay of anything)
 */
void linenoiseClearScreen( void ) {
#ifdef _WIN32
    COORD coord = {0, 0};
    CONSOLE_SCREEN_BUFFER_INFO inf;
    HANDLE screenHandle = GetStdHandle( STD_OUTPUT_HANDLE );
    GetConsoleScreenBufferInfo( screenHandle, &inf );
    SetConsoleCursorPosition( screenHandle, coord );
    DWORD count;
    FillConsoleOutputCharacterA( screenHandle, ' ', inf.dwSize.X * inf.dwSize.Y, coord, &count );
#else
    if ( write( 1, "\x1b[H\x1b[2J", 7 ) <= 0 ) return;
#endif
}

void InputBuffer::clearScreen( PromptBase& pi ) {
    linenoiseClearScreen();
    if ( write32( 1, pi.promptText.get(), pi.promptChars ) == -1 ) return;
#ifndef _WIN32
    // we have to generate our own newline on line wrap on Linux
    if ( pi.promptIndentation == 0 && pi.promptExtraLines > 0 )
        if ( write( 1, "\n", 1 ) == -1 ) return;
#endif
    pi.promptCursorRowOffset = pi.promptExtraLines;
    refreshLine( pi );
}

/**
 * Incremental history search -- take over the prompt and keyboard as the user types a search string,
 * deletes characters from it, changes direction, and either accepts the found line (for execution or
 * editing) or cancels.
 * @param pi        PromptBase struct holding information about the (old, static) prompt and our screen position
 * @param startChar the character that began the search, used to set the initial direction
 */
int InputBuffer::incrementalHistorySearch( PromptBase& pi, int startChar ) {
    size_t bufferSize;
    size_t ucharCount;
    int errorCode;

    // if not already recalling, add the current line to the history list so we don't have to special case it
    if ( historyIndex == historyLen - 1 ) {
        free( history[historyLen - 1] );
        bufferSize = sizeof( UChar32 ) * len + 1;
        scoped_array< UChar8 > tempBuffer( new UChar8[ bufferSize ] );
        copyString32to8( tempBuffer.get(), buf32, bufferSize );
        history[ historyLen - 1 ] = reinterpret_cast< UChar8 * >( strdup( reinterpret_cast< const char * >( tempBuffer.get() ) ) );
    }
    int historyLineLength = len;
    int historyLinePosition = pos;
    UChar32 emptyBuffer[1];
    char emptyWidths[1];
    InputBuffer empty( emptyBuffer, emptyWidths, 1 );
    empty.refreshLine( pi );                        // erase the old input first
    DynamicPrompt dp( pi, ( startChar == ctrlChar( 'R' ) ) ? -1 : 1 );

    dp.promptPreviousLen = pi.promptPreviousLen;
    dp.promptPreviousInputLen = pi.promptPreviousInputLen;
    dynamicRefresh( dp, buf32, historyLineLength, historyLinePosition ); // draw user's text with our prompt

    // loop until we get an exit character
    int c;
    bool keepLooping = true;
    bool useSearchedLine = true;
    bool searchAgain = false;
    UChar32* activeHistoryLine = 0;
    while ( keepLooping ) {
        c = linenoiseReadChar();
        c = cleanupCtrl( c );           // convert CTRL + <char> into normal ctrl

        switch ( c ) {

        // these characters keep the selected text but do not execute it
        case ctrlChar( 'A' ):   // ctrl-A, move cursor to start of line
        case HOME_KEY:
        case ctrlChar( 'B' ):   // ctrl-B, move cursor left by one character
        case LEFT_ARROW_KEY:
        case META + 'b':        // meta-B, move cursor left by one word
        case META + 'B':
        case CTRL + LEFT_ARROW_KEY:
        case META + LEFT_ARROW_KEY: // Emacs allows Meta, bash & readline don't
        case ctrlChar( 'D' ):
        case META + 'd':        // meta-D, kill word to right of cursor
        case META + 'D':
        case ctrlChar( 'E' ):   // ctrl-E, move cursor to end of line
        case END_KEY:
        case ctrlChar( 'F' ):   // ctrl-F, move cursor right by one character
        case RIGHT_ARROW_KEY:
        case META + 'f':        // meta-F, move cursor right by one word
        case META + 'F':
        case CTRL + RIGHT_ARROW_KEY:
        case META + RIGHT_ARROW_KEY: // Emacs allows Meta, bash & readline don't
        case META + ctrlChar( 'H' ):
        case ctrlChar( 'J' ):
        case ctrlChar( 'K' ):   // ctrl-K, kill from cursor to end of line
        case ctrlChar( 'M' ):
        case ctrlChar( 'N' ):   // ctrl-N, recall next line in history
        case ctrlChar( 'P' ):   // ctrl-P, recall previous line in history
        case DOWN_ARROW_KEY:
        case UP_ARROW_KEY:
        case ctrlChar( 'T' ):   // ctrl-T, transpose characters
        case ctrlChar( 'U' ):   // ctrl-U, kill all characters to the left of the cursor
        case ctrlChar( 'W' ):
        case META + 'y':        // meta-Y, "yank-pop", rotate popped text
        case META + 'Y':
        case 127:
        case DELETE_KEY:
        case META + '<':        // start of history
        case PAGE_UP_KEY:
        case META + '>':        // end of history
        case PAGE_DOWN_KEY:
            keepLooping = false;
            break;

        // these characters revert the input line to its previous state
        case ctrlChar( 'C' ):   // ctrl-C, abort this line
        case ctrlChar( 'G' ):
        case ctrlChar( 'L' ):   // ctrl-L, clear screen and redisplay line
            keepLooping = false;
            useSearchedLine = false;
            if ( c != ctrlChar( 'L' ) ) {
                c = -1;         // ctrl-C and ctrl-G just abort the search and do nothing else
            }
            break;

        // these characters stay in search mode and update the display
        case ctrlChar( 'S' ):
        case ctrlChar( 'R' ):
            if ( dp.searchTextLen == 0 ) {  // if no current search text, recall previous text
                if ( previousSearchText.length() ) {
                    dp.updateSearchText( previousSearchText.get() );
                }
            }
            if ( ( dp.direction ==  1 && c == ctrlChar( 'R' ) ) ||
                 ( dp.direction == -1 && c == ctrlChar( 'S' ) ) ) {
                dp.direction = 0 - dp.direction;    // reverse direction
                dp.updateSearchPrompt();            // change the prompt
            }
            else {
                searchAgain = true;                 // same direction, search again
            }
            break;

            // job control is its own thing
#ifndef _WIN32
        case ctrlChar( 'Z' ):   // ctrl-Z, job control
            disableRawMode();                       // Returning to Linux (whatever) shell, leave raw mode
            raise( SIGSTOP );                       // Break out in mid-line
            enableRawMode();                        // Back from Linux shell, re-enter raw mode
            {
                bufferSize = historyLineLength + 1;
                scoped_array< UChar32 > tempUnicode( new UChar32[ bufferSize ] );
                copyString8to32( tempUnicode.get(), history[ historyIndex ], bufferSize, ucharCount, errorCode );
                dynamicRefresh( dp, tempUnicode.get(), historyLineLength, historyLinePosition );
            }
            continue;
            break;
#endif

        // these characters update the search string, and hence the selected input line
        case ctrlChar( 'H' ):   // backspace/ctrl-H, delete char to left of cursor
            if ( dp.searchTextLen > 0 ) {
                scoped_array<UChar32> tempUnicode( new UChar32[ dp.searchTextLen ] );
                --dp.searchTextLen;
                dp.searchText[ dp.searchTextLen ] = 0;
                copyString32( tempUnicode.get(), dp.searchText.get(), dp.searchTextLen + 1 );
                dp.updateSearchText( tempUnicode.get() );
            }
            else {
                beep();
            }
            break;

        case ctrlChar( 'Y' ):   // ctrl-Y, yank killed text
            break;

        default:
            if ( !isControlChar( c ) && c <= 0x0010FFFF ) {    // not an action character
                scoped_array< UChar32 > tempUnicode( new UChar32[ dp.searchTextLen + 2 ] );
                copyString32( tempUnicode.get(), dp.searchText.get(), dp.searchTextLen + 2 );
                tempUnicode[ dp.searchTextLen ] = c;
                tempUnicode[ dp.searchTextLen + 1 ] = 0;
                dp.updateSearchText( tempUnicode.get() );
            }
            else {
                beep();
            }
        } // switch

        // if we are staying in search mode, search now
        if ( keepLooping ) {
            bufferSize = historyLineLength + 1;
            activeHistoryLine = new UChar32[ bufferSize ];
            copyString8to32( activeHistoryLine, history[ historyIndex ], bufferSize, ucharCount, errorCode );
            if ( dp.searchTextLen > 0 ) {
                bool found = false;
                int historySearchIndex = historyIndex;
                int lineLength = ucharCount;
                int lineSearchPos = historyLinePosition;
                if ( searchAgain ) {
                    lineSearchPos += dp.direction;
                }
                searchAgain = false;
                while ( true ) {
                    while ( ( dp.direction > 0 ) ? ( lineSearchPos < lineLength ) : ( lineSearchPos >= 0 ) ) {
                        if ( strncmp32( dp.searchText.get(), &activeHistoryLine[ lineSearchPos ], dp.searchTextLen) == 0 ) {
                            found = true;
                            break;
                        }
                        lineSearchPos += dp.direction;
                    }
                    if ( found ) {
                        historyIndex = historySearchIndex;
                        historyLineLength = lineLength;
                        historyLinePosition = lineSearchPos;
                        break;
                    }
                    else if ( ( dp.direction > 0 ) ? ( historySearchIndex < historyLen - 1 ) : ( historySearchIndex > 0 ) ) {
                        historySearchIndex += dp.direction;
                        bufferSize = strlen( reinterpret_cast< char* >( history[ historySearchIndex ] ) ) + 1;
                        delete [] activeHistoryLine;
                        activeHistoryLine = new UChar32[ bufferSize ];
                        copyString8to32( activeHistoryLine, history[ historySearchIndex ], bufferSize, ucharCount, errorCode );
                        lineLength = ucharCount;
                        lineSearchPos = ( dp.direction > 0 ) ? 0 : ( lineLength - dp.searchTextLen );
                    }
                    else {
                        beep();
                        break;
                    }
                }; // while
            }
            if ( activeHistoryLine ) {
                delete [] activeHistoryLine;
            }
            bufferSize = historyLineLength + 1;
            activeHistoryLine = new UChar32[ bufferSize ];
            copyString8to32( activeHistoryLine, history[ historyIndex ], bufferSize, ucharCount, errorCode );
            dynamicRefresh( dp, activeHistoryLine, historyLineLength, historyLinePosition ); // draw user's text with our prompt
        }
    } // while

    // leaving history search, restore previous prompt, maybe make searched line current
    PromptBase pb;
    pb.promptChars = pi.promptIndentation;
    Utf32String tempUnicode( pb.promptChars + 1 );
    copyString32( tempUnicode.get(), &pi.promptText[ pi.promptLastLinePosition ], pb.promptChars + 1 );
    tempUnicode.initFromBuffer();
    pb.promptText = tempUnicode;
    pb.promptExtraLines = 0;
    pb.promptIndentation = pi.promptIndentation;
    pb.promptLastLinePosition = 0;
    pb.promptPreviousInputLen = historyLineLength;
    pb.promptCursorRowOffset = dp.promptCursorRowOffset;
    pb.promptScreenColumns = pi.promptScreenColumns;
    pb.promptPreviousLen = dp.promptChars;
    if ( useSearchedLine ) {
        historyRecallMostRecent = true;
        copyString32( buf32, activeHistoryLine, buflen + 1 );
        len = historyLineLength;
        pos = historyLinePosition;
    }
    if ( activeHistoryLine ) {
        delete [] activeHistoryLine;
    }
    dynamicRefresh( pb, buf32, len, pos );  // redraw the original prompt with current input
    pi.promptPreviousInputLen = len;
    pi.promptCursorRowOffset = pi.promptExtraLines + pb.promptCursorRowOffset;
    previousSearchText = dp.searchText;     // save search text for possible reuse on ctrl-R ctrl-R
    return c;                               // pass a character or -1 back to main loop
}

static bool isCharacterAlphanumeric( UChar32 testChar ) {
    return iswalnum( testChar );
}

int InputBuffer::getInputLine( PromptBase& pi ) {

    // The latest history entry is always our current buffer
    if ( len > 0 ) {
        size_t bufferSize = sizeof( UChar32 ) * len + 1;
        scoped_array< char > tempBuffer( new char[ bufferSize ] );
        copyString32to8( reinterpret_cast< UChar8 * >( tempBuffer.get() ), buf32, bufferSize );
        linenoiseHistoryAdd( tempBuffer.get() );
    }
    else {
        linenoiseHistoryAdd( "" );
    }
    historyIndex = historyLen - 1;
    historyRecallMostRecent = false;

    // display the prompt
    if ( write32( 1, pi.promptText.get(), pi.promptChars ) == -1 ) return -1;

#ifndef _WIN32
    // we have to generate our own newline on line wrap on Linux
    if ( pi.promptIndentation == 0 && pi.promptExtraLines > 0 )
        if ( write( 1, "\n", 1 ) == -1 ) return -1;
#endif

    // the cursor starts out at the end of the prompt
    pi.promptCursorRowOffset = pi.promptExtraLines;

    // kill and yank start in "other" mode
    killRing.lastAction = KillRing::actionOther;

    // when history search returns control to us, we execute its terminating keystroke
    int terminatingKeystroke = -1;

    // if there is already text in the buffer, display it first
    if ( len > 0 ) {
        refreshLine( pi );
    }

    // loop collecting characters, respond to line editing characters
    while ( true ) {
        int c;
        if ( terminatingKeystroke == -1 ) {
            c = linenoiseReadChar();    // get a new keystroke
        }
        else {
            c = terminatingKeystroke;   // use the terminating keystroke from search
            terminatingKeystroke = -1;  // clear it once we've used it
        }
        c = cleanupCtrl( c );           // convert CTRL + <char> into normal ctrl

        if ( c == 0 ) {
            return len;
        }

        if ( c == -1 ) {
            refreshLine( pi );
            continue;
        }

        if ( c == -2 ) {
            if ( write32( 1, pi.promptText.get(), pi.promptChars ) == -1 ) return -1;
            refreshLine( pi );
            continue;
        }

        // ctrl-I/tab, command completion, needs to be before switch statement
        if ( c == ctrlChar( 'I' ) && completionCallback ) {

            if ( pos == 0 )             // SERVER-4967 -- in earlier versions, you could paste previous output
                continue;               //  back into the shell ... this output may have leading tabs.
                                        // This hack (i.e. what the old code did) prevents command completion
                                        //  on an empty line but lets users paste text with leading tabs.

            killRing.lastAction = KillRing::actionOther;
            historyRecallMostRecent = false;

            // completeLine does the actual completion and replacement
            c = completeLine( pi );

            if ( c < 0 )                // return on error
                return len;

            if ( c == 0 )               // read next character when 0
                continue;

            // deliberate fall-through here, so we use the terminating character
        }

        switch ( c ) {

        case ctrlChar( 'A' ):   // ctrl-A, move cursor to start of line
        case HOME_KEY:
            killRing.lastAction = KillRing::actionOther;
            pos = 0;
            refreshLine( pi );
            break;

        case ctrlChar( 'B' ):   // ctrl-B, move cursor left by one character
        case LEFT_ARROW_KEY:
            killRing.lastAction = KillRing::actionOther;
            if ( pos > 0 ) {
                --pos;
                refreshLine( pi );
            }
            break;

        case META + 'b':        // meta-B, move cursor left by one word
        case META + 'B':
        case CTRL + LEFT_ARROW_KEY:
        case META + LEFT_ARROW_KEY: // Emacs allows Meta, bash & readline don't
            killRing.lastAction = KillRing::actionOther;
            if ( pos > 0 ) {
                while ( pos > 0 && !isCharacterAlphanumeric( buf32[pos - 1] ) ) {
                    --pos;
                }
                while ( pos > 0 && isCharacterAlphanumeric( buf32[pos - 1] ) ) {
                    --pos;
                }
                refreshLine( pi );
            }
            break;

        case ctrlChar( 'C' ):   // ctrl-C, abort this line
            killRing.lastAction = KillRing::actionOther;
            historyRecallMostRecent = false;
            errno = EAGAIN;
            --historyLen;
            free( history[historyLen] );
            // we need one last refresh with the cursor at the end of the line
            // so we don't display the next prompt over the previous input line
            pos = len;  // pass len as pos for EOL
            refreshLine( pi );
            if ( write( 1, "^C", 2 ) == -1 ) return -1;    // Display the ^C we got
            return -1;

        case META + 'c':        // meta-C, give word initial Cap
        case META + 'C':
            killRing.lastAction = KillRing::actionOther;
            historyRecallMostRecent = false;
            if ( pos < len ) {
                while ( pos < len && !isCharacterAlphanumeric( buf32[pos] ) ) {
                    ++pos;
                }
                if ( pos < len && isCharacterAlphanumeric( buf32[pos] ) ) {
                    if ( buf32[pos] >= 'a' && buf32[pos] <= 'z' ) {
                        buf32[pos] += 'A' - 'a';
                    }
                    ++pos;
                }
                while ( pos < len && isCharacterAlphanumeric( buf32[pos] ) ) {
                    if ( buf32[pos] >= 'A' && buf32[pos] <= 'Z' ) {
                        buf32[pos] += 'a' - 'A';
                    }
                    ++pos;
                }
                refreshLine( pi );
            }
            break;

        // ctrl-D, delete the character under the cursor
        // on an empty line, exit the shell
        case ctrlChar( 'D' ):
            killRing.lastAction = KillRing::actionOther;
            if ( len > 0 && pos < len ) {
                historyRecallMostRecent = false;
                memmove( buf32 + pos, buf32 + pos + 1, sizeof( UChar32 ) * ( len - pos ) );
                --len;
                refreshLine( pi );
            }
            else if ( len == 0 ) {
                --historyLen;
                free( history[historyLen] );
                return -1;
            }
            break;

        case META + 'd':        // meta-D, kill word to right of cursor
        case META + 'D':
            if ( pos < len ) {
                historyRecallMostRecent = false;
                int endingPos = pos;
                while ( endingPos < len && !isCharacterAlphanumeric( buf32[endingPos] ) ) {
                    ++endingPos;
                }
                while ( endingPos < len && isCharacterAlphanumeric( buf32[endingPos] ) ) {
                    ++endingPos;
                }
                killRing.kill( &buf32[pos], endingPos - pos, true );
                memmove( buf32 + pos, buf32 + endingPos, sizeof( UChar32 ) * ( len - endingPos + 1 ) );
                len -= endingPos - pos;
                refreshLine( pi );
            }
            killRing.lastAction = KillRing::actionKill;
            break;

        case ctrlChar( 'E' ):   // ctrl-E, move cursor to end of line
        case END_KEY:
            killRing.lastAction = KillRing::actionOther;
            pos = len;
            refreshLine( pi );
            break;

        case ctrlChar( 'F' ):   // ctrl-F, move cursor right by one character
        case RIGHT_ARROW_KEY:
            killRing.lastAction = KillRing::actionOther;
            if ( pos < len ) {
                ++pos;
                refreshLine( pi );
            }
            break;

        case META + 'f':        // meta-F, move cursor right by one word
        case META + 'F':
        case CTRL + RIGHT_ARROW_KEY:
        case META + RIGHT_ARROW_KEY: // Emacs allows Meta, bash & readline don't
            killRing.lastAction = KillRing::actionOther;
            if ( pos < len ) {
                while ( pos < len && !isCharacterAlphanumeric( buf32[pos] ) ) {
                    ++pos;
                }
                while ( pos < len && isCharacterAlphanumeric( buf32[pos] ) ) {
                    ++pos;
                }
                refreshLine( pi );
            }
            break;

        case ctrlChar( 'H' ):   // backspace/ctrl-H, delete char to left of cursor
            killRing.lastAction = KillRing::actionOther;
            if ( pos > 0 ) {
                historyRecallMostRecent = false;
                memmove( buf32 + pos - 1, buf32 + pos, sizeof( UChar32 ) * ( 1 + len - pos ) );
                --pos;
                --len;
                refreshLine( pi );
            }
            break;

        // meta-Backspace, kill word to left of cursor
        case META + ctrlChar( 'H' ):
            if ( pos > 0 ) {
                historyRecallMostRecent = false;
                int startingPos = pos;
                while ( pos > 0 && !isCharacterAlphanumeric( buf32[pos - 1] ) ) {
                    --pos;
                }
                while ( pos > 0 && isCharacterAlphanumeric( buf32[pos - 1] ) ) {
                    --pos;
                }
                killRing.kill( &buf32[pos], startingPos - pos, false );
                memmove( buf32 + pos, buf32 + startingPos, sizeof( UChar32 ) * ( len - startingPos + 1 ) );
                len -= startingPos - pos;
                refreshLine( pi );
            }
            killRing.lastAction = KillRing::actionKill;
            break;

        case ctrlChar( 'J' ):   // ctrl-J/linefeed/newline, accept line
        case ctrlChar( 'M' ):   // ctrl-M/return/enter
            killRing.lastAction = KillRing::actionOther;
            // we need one last refresh with the cursor at the end of the line
            // so we don't display the next prompt over the previous input line
            pos = len;    // pass len as pos for EOL
            refreshLine( pi );
            historyPreviousIndex = historyRecallMostRecent ? historyIndex : -2;
            --historyLen;
            free( history[historyLen] );
            return len;

        case ctrlChar( 'K' ):   // ctrl-K, kill from cursor to end of line
            killRing.kill( &buf32[pos], len - pos, true );
            buf32[pos] = '\0';
            len = pos;
            refreshLine( pi );
            killRing.lastAction = KillRing::actionKill;
            historyRecallMostRecent = false;
            break;

        case ctrlChar( 'L' ):   // ctrl-L, clear screen and redisplay line
            clearScreen( pi );
            break;

        case META + 'l':        // meta-L, lowercase word
        case META + 'L':
            killRing.lastAction = KillRing::actionOther;
            if ( pos < len ) {
                historyRecallMostRecent = false;
                while ( pos < len && !isCharacterAlphanumeric( buf32[pos] ) ) {
                    ++pos;
                }
                while ( pos < len && isCharacterAlphanumeric( buf32[pos] ) ) {
                    if ( buf32[pos] >= 'A' && buf32[pos] <= 'Z' ) {
                        buf32[pos] += 'a' - 'A';
                    }
                    ++pos;
                }
                refreshLine( pi );
            }
            break;

        case ctrlChar( 'N' ):   // ctrl-N, recall next line in history
        case ctrlChar( 'P' ):   // ctrl-P, recall previous line in history
        case DOWN_ARROW_KEY:
        case UP_ARROW_KEY:
            killRing.lastAction = KillRing::actionOther;
            // if not already recalling, add the current line to the history list so we don't have to special case it
            if ( historyIndex == historyLen - 1 ) {
                free( history[historyLen - 1] );
                size_t tempBufferSize = sizeof( UChar32 ) * len + 1;
                scoped_array< UChar8 > tempBuffer( new UChar8[ tempBufferSize ] );
                copyString32to8( tempBuffer.get(), buf32, tempBufferSize );
                history[historyLen - 1] = reinterpret_cast< UChar8 * >( strdup( reinterpret_cast< const char * >( tempBuffer.get() ) ) );
            }
            if ( historyLen > 1 ) {
                if ( c == UP_ARROW_KEY ) {
                    c = ctrlChar( 'P' );
                }
                if ( historyPreviousIndex != -2 && c != ctrlChar( 'P' ) ) {
                    historyIndex = 1 + historyPreviousIndex;    // emulate Windows down-arrow
                }
                else {
                    historyIndex += ( c == ctrlChar( 'P' ) ) ? -1 : 1;
                }
                historyPreviousIndex = -2;
                if ( historyIndex < 0 ) {
                    historyIndex = 0;
                    break;
                }
                else if ( historyIndex >= historyLen ) {
                    historyIndex = historyLen - 1;
                    break;
                }
                historyRecallMostRecent = true;
                size_t ucharCount;
                int errorCode;
                copyString8to32( buf32, history[historyIndex], buflen, ucharCount, errorCode );
                len = pos = ucharCount;
                refreshLine( pi );
            }
            break;

        case ctrlChar( 'R' ):   // ctrl-R, reverse history search
        case ctrlChar( 'S' ):   // ctrl-S, forward history search
            terminatingKeystroke = incrementalHistorySearch( pi, c );
            break;

        case ctrlChar( 'T' ):   // ctrl-T, transpose characters
            killRing.lastAction = KillRing::actionOther;
            if ( pos > 0 && len > 1 ) {
                historyRecallMostRecent = false;
                size_t leftCharPos = ( pos == len ) ? pos - 2 : pos - 1;
                char aux = buf32[leftCharPos];
                buf32[leftCharPos] = buf32[leftCharPos+1];
                buf32[leftCharPos+1] = aux;
                if ( pos != len )
                    ++pos;
                refreshLine( pi );
            }
            break;

        case ctrlChar( 'U' ):   // ctrl-U, kill all characters to the left of the cursor
            if ( pos > 0 ) {
                historyRecallMostRecent = false;
                killRing.kill( &buf32[0], pos, false );
                len -= pos;
                memmove( buf32, buf32 + pos, sizeof( UChar32 ) * ( len + 1 ) );
                pos = 0;
                refreshLine( pi );
            }
            killRing.lastAction = KillRing::actionKill;
            break;

        case META + 'u':        // meta-U, uppercase word
        case META + 'U':
            killRing.lastAction = KillRing::actionOther;
            if ( pos < len ) {
                historyRecallMostRecent = false;
                while ( pos < len && !isCharacterAlphanumeric( buf32[pos] ) ) {
                    ++pos;
                }
                while ( pos < len && isCharacterAlphanumeric( buf32[pos] ) ) {
                    if ( buf32[pos] >= 'a' && buf32[pos] <= 'z' ) {
                        buf32[pos] += 'A' - 'a';
                    }
                    ++pos;
                }
                refreshLine( pi );
            }
            break;

        // ctrl-W, kill to whitespace (not word) to left of cursor
        case ctrlChar( 'W' ):
            if ( pos > 0 ) {
                historyRecallMostRecent = false;
                int startingPos = pos;
                while ( pos > 0 && buf32[pos - 1] == ' ' ) {
                    --pos;
                }
                while ( pos > 0 && buf32[pos - 1] != ' ' ) {
                    --pos;
                }
                killRing.kill( &buf32[pos], startingPos - pos, false );
                memmove( buf32 + pos, buf32 + startingPos, sizeof( UChar32 ) * ( len - startingPos + 1 ) );
                len -= startingPos - pos;
                refreshLine( pi );
            }
            killRing.lastAction = KillRing::actionKill;
            break;

        case ctrlChar( 'Y' ):   // ctrl-Y, yank killed text
            historyRecallMostRecent = false;
            {
                Utf32String* restoredText = killRing.yank();
                if ( restoredText ) {
                    size_t ucharCount = restoredText->length();
                    memmove( buf32 + pos + ucharCount, buf32 + pos, sizeof( UChar32 ) * ( len - pos + 1 ) );
                    memmove( buf32 + pos, restoredText->get(), sizeof( UChar32 ) * ucharCount );
                    pos += ucharCount;
                    len += ucharCount;
                    refreshLine( pi );
                    killRing.lastAction = KillRing::actionYank;
                    killRing.lastYankSize = ucharCount;
                }
                else {
                    beep();
                }
            }
            break;

        case META + 'y':        // meta-Y, "yank-pop", rotate popped text
        case META + 'Y':
            if ( killRing.lastAction == KillRing::actionYank ) {
                historyRecallMostRecent = false;
                Utf32String* restoredText = killRing.yankPop();
                if ( restoredText ) {
                    size_t ucharCount = restoredText->length();
                    if ( ucharCount > killRing.lastYankSize ) {
                        memmove( buf32 + pos + ucharCount - killRing.lastYankSize, buf32 + pos, sizeof( UChar32 ) * ( len - pos + 1 ) );
                        memmove( buf32 + pos - killRing.lastYankSize, restoredText->get(), sizeof( UChar32 ) * ucharCount );
                    }
                    else {
                        memmove( buf32 + pos - killRing.lastYankSize, restoredText->get(), sizeof( UChar32 ) * ucharCount );
                        memmove( buf32 + pos + ucharCount - killRing.lastYankSize, buf32 + pos, sizeof( UChar32 ) * ( len - pos + 1 ) );
                    }
                    pos += ucharCount - killRing.lastYankSize;
                    len += ucharCount - killRing.lastYankSize;
                    killRing.lastYankSize = ucharCount;
                    refreshLine( pi );
                    break;
                }
            }
            beep();
            break;

#ifndef _WIN32
        case ctrlChar( 'Z' ):   // ctrl-Z, job control
            disableRawMode();                       // Returning to Linux (whatever) shell, leave raw mode
            raise( SIGSTOP );                       // Break out in mid-line
            enableRawMode();                        // Back from Linux shell, re-enter raw mode
            if ( write32( 1, pi.promptText.get(), pi.promptChars ) == -1 ) break; // Redraw prompt
            refreshLine( pi );                      // Refresh the line
            break;
#endif

        // DEL, delete the character under the cursor
        case 127:
        case DELETE_KEY:
            killRing.lastAction = KillRing::actionOther;
            if ( len > 0 && pos < len ) {
                historyRecallMostRecent = false;
                memmove( buf32 + pos, buf32 + pos + 1, sizeof( UChar32 ) * ( len - pos ) );
                --len;
                refreshLine( pi );
            }
            break;

        case META + '<':        // meta-<, beginning of history
        case PAGE_UP_KEY:       // Page Up, beginning of history
        case META + '>':        // meta->, end of history
        case PAGE_DOWN_KEY:     // Page Down, end of history
            killRing.lastAction = KillRing::actionOther;
            // if not already recalling, add the current line to the history list so we don't have to special case it
            if ( historyIndex == historyLen - 1 ) {
                free( history[historyLen - 1] );
                size_t tempBufferSize = sizeof( UChar32 ) * len + 1;
                scoped_array< UChar8 > tempBuffer( new UChar8[ tempBufferSize ] );
                copyString32to8( tempBuffer.get(), buf32, tempBufferSize );
                history[historyLen - 1] = reinterpret_cast< UChar8 * >( strdup( reinterpret_cast< const char * >( tempBuffer.get() ) ) );
            }
            if ( historyLen > 1 ) {
                historyIndex = ( c == META + '<' || c == PAGE_UP_KEY ) ? 0 : historyLen - 1;
                historyPreviousIndex = -2;
                historyRecallMostRecent = true;
                size_t ucharCount;
                int errorCode;
                copyString8to32( buf32, history[historyIndex], buflen, ucharCount, errorCode );
                len = pos = ucharCount;
                refreshLine( pi );
            }
            break;

        // not one of our special characters, maybe insert it in the buffer
        default:
            killRing.lastAction = KillRing::actionOther;
            historyRecallMostRecent = false;
            if ( c & ( META | CTRL ) ) {   // beep on unknown Ctrl and/or Meta keys
                beep();
                break;
            }
            if ( len < buflen ) {
                if ( isControlChar( c ) ) { // don't insert control characters
                    beep();
                    break;
                }
                if ( len == pos ) {         // at end of buffer
                    buf32[pos] = c;
                    ++pos;
                    ++len;
                    buf32[len] = '\0';
                    if ( pi.promptIndentation + len < pi.promptScreenColumns ) {
                        if ( len > pi.promptPreviousInputLen )
                            pi.promptPreviousInputLen = len;
                        /* Avoid a full update of the line in the
                         * trivial case. */
                        if ( write32( 1, reinterpret_cast<UChar32 *>( &c ), 1) == -1 ) return -1;
                    }
                    else {
                        refreshLine( pi );
                    }
                }
                else {  // not at end of buffer, have to move characters to our right
                    memmove( buf32 + pos + 1, buf32 + pos, sizeof( UChar32 ) * ( len - pos ) );
                    buf32[pos] = c;
                    ++len;
                    ++pos;
                    buf32[len] = '\0';
                    refreshLine( pi );
                }
            }
            else {
                beep();     // buffer is full, beep on new characters
            }
            break;
        }
    }
    return len;
}

string preloadedBufferContents;     // used with linenoisePreloadBuffer
string preloadErrorMessage;

/**
 * linenoisePreloadBuffer provides text to be inserted into the command buffer
 *
 * the provided text will be processed to be usable and will be used to preload
 * the input buffer on the next call to linenoise()
 *
 * @param preloadText text to begin with on the next call to linenoise()
 */
void linenoisePreloadBuffer( const char* preloadText ) {

    if ( ! preloadText ) {
        return;
    }
    int bufferSize = strlen( preloadText ) + 1;
    scoped_array< char > tempBuffer( new char[ bufferSize ] );
    strncpy( &tempBuffer[0], preloadText, bufferSize );

    // remove characters that won't display correctly
    char* pIn = &tempBuffer[0];
    char* pOut = pIn;
    bool controlsStripped = false;
    bool whitespaceSeen = false;
    while ( *pIn ) {
        unsigned char c = *pIn++;       // we need unsigned so chars 0x80 and above are allowed
        if ( '\r' == c ) {              // silently skip CR
            continue;
        }
        if ( '\n' == c || '\t' == c ) { // note newline or tab
            whitespaceSeen = true;
            continue;
        }
        if ( isControlChar( c ) ) {     // remove other control characters, flag for message
            controlsStripped = true;
            *pOut++ = ' ';
            continue;
        }
        if ( whitespaceSeen ) {         // convert whitespace to a single space
            *pOut++ = ' ';
            whitespaceSeen = false;
        }
        *pOut++ = c;
    }
    *pOut = 0;
    int processedLength = pOut - tempBuffer.get();
    bool lineTruncated = false;
    if ( processedLength > ( LINENOISE_MAX_LINE - 1 ) ) {
        lineTruncated = true;
        tempBuffer[ LINENOISE_MAX_LINE - 1 ] = 0;
    }
    preloadedBufferContents = tempBuffer.get();
    if ( controlsStripped ) {
        preloadErrorMessage += " [Edited line: control characters were converted to spaces]\n";
    }
    if ( lineTruncated ) {
        preloadErrorMessage += " [Edited line: the line length was reduced from ";
        char buf[128];
        snprintf( buf, sizeof( buf ), "%d to %d]\n", processedLength, ( LINENOISE_MAX_LINE - 1 ) );
        preloadErrorMessage += buf;
    }
}

/**
 * linenoise is a readline replacement.
 *
 * call it with a prompt to display and it will return a line of input from the user
 *
 * @param prompt text of prompt to display to the user
 * @return       the returned string belongs to the caller on return and must be freed to prevent memory leaks
 */
char* linenoise( const char* prompt ) {
    if ( isatty( STDIN_FILENO ) ) {             // input is from a terminal
        UChar32 buf32[ LINENOISE_MAX_LINE ];
        char charWidths[ LINENOISE_MAX_LINE ];
        if ( ! preloadErrorMessage.empty() ) {
            printf( "%s", preloadErrorMessage.c_str() );
            fflush( stdout );
            preloadErrorMessage.clear();
        }
        PromptInfo pi( reinterpret_cast< const UChar8* >( prompt ), getScreenColumns() );
        if ( isUnsupportedTerm() ) {
            if ( write32( 1, pi.promptText.get(), pi.promptChars ) == -1 ) return 0;
            fflush( stdout );
            if ( preloadedBufferContents.empty() ) {
                scoped_array<char> buf8( new char[ LINENOISE_MAX_LINE ] );
                if ( fgets( buf8.get(), LINENOISE_MAX_LINE, stdin ) == NULL ) {
                    return NULL;
                }
                size_t len = strlen( buf8.get() );
                while ( len && ( buf8[len - 1] == '\n' || buf8[len - 1] == '\r' ) ) {
                    --len;
                    buf8[len] = '\0';
                }
                return strdup( buf8.get() );    // caller must free buffer
            }
            else {
                char* buf8 = strdup( preloadedBufferContents.c_str() );
                preloadedBufferContents.clear();
                return buf8;                    // caller must free buffer
            }
        }
        else {
            if ( enableRawMode() == -1 ) {
                return NULL;
            }
            InputBuffer ib( buf32, charWidths, LINENOISE_MAX_LINE );
            if ( ! preloadedBufferContents.empty() ) {
                ib.preloadBuffer( reinterpret_cast< const UChar8 * >( preloadedBufferContents.c_str() ) );
                preloadedBufferContents.clear();
            }
            int count = ib.getInputLine( pi );
            disableRawMode();
            printf( "\n" );
            if ( count == -1 ) {
                return NULL;
            }
            size_t bufferSize = sizeof( UChar32 ) * ib.length() + 1;
            scoped_array<UChar8> buf8( new UChar8[ bufferSize ] );
            copyString32to8( buf8.get(), buf32, bufferSize );
            return strdup( reinterpret_cast<char*>( buf8.get() ) ); // caller must free buffer
        }
    }
    else {  // input not from a terminal, we should work with piped input, i.e. redirected stdin
        scoped_array<char> buf8( new char[ LINENOISE_MAX_LINE ] );
        if ( fgets( buf8.get(), LINENOISE_MAX_LINE, stdin ) == NULL ) {
            return NULL;
        }

        // if fgets() gave us the newline, remove it
        int count = strlen( buf8.get() );
        if ( count > 0 && buf8[ count - 1 ] == '\n' ) {
            --count;
            buf8[ count ] = '\0';
        }
        return strdup( buf8.get() );    // caller must free buffer
    }
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback( linenoiseCompletionCallback* fn ) {
    completionCallback = fn;
}

void linenoiseAddCompletion( linenoiseCompletions* lc, const char* str ) {
    lc->completionStrings.push_back( Utf32String( reinterpret_cast<const UChar8*>( str ) ) );
}

int linenoiseHistoryAdd( const char* line ) {
    if ( historyMaxLen == 0 ) {
        return 0;
    }
    if ( history == NULL ) {
        history = reinterpret_cast< UChar8** >( malloc( sizeof( UChar8* ) * historyMaxLen ) );
        if (history == NULL) {
            return 0;
        }
        memset( history, 0, ( sizeof( char* ) * historyMaxLen ) );
    }
    UChar8* linecopy = reinterpret_cast< UChar8* >( strdup( line ) );
    if ( ! linecopy ) {
        return 0;
    }
    if ( historyLen == historyMaxLen ) {
        free( history[0] );
        memmove( history, history + 1, sizeof( char* ) * ( historyMaxLen - 1 ) );
        --historyLen;
        if ( --historyPreviousIndex < -1 ) {
            historyPreviousIndex = -2;
        }
    }

    // convert newlines in multi-line code to spaces before storing
    UChar8* p = linecopy;
    while ( *p ) {
        if ( *p == '\n' ) {
            *p = ' ';
        }
        ++p;
    }
    history[historyLen] = linecopy;
    ++historyLen;
    return 1;
}

int linenoiseHistorySetMaxLen( int len ) {
    if ( len < 1 ) {
        return 0;
    }
    if ( history ) {
        int tocopy = historyLen;
        UChar8** newHistory = reinterpret_cast< UChar8** >( malloc( sizeof( UChar8* ) * len ) );
        if ( newHistory == NULL ) {
            return 0;
        }
        if ( len < tocopy ) {
            tocopy = len;
        }
        memcpy( newHistory, history + historyMaxLen - tocopy, sizeof( UChar8* ) * tocopy );
        free( history );
        history = newHistory;
    }
    historyMaxLen = len;
    if ( historyLen > historyMaxLen ) {
        historyLen = historyMaxLen;
    }
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave( const char* filename ) {
    FILE* fp = fopen( filename, "wt" );
    if ( fp == NULL ) {
        return -1;
    }

    for ( int j = 0; j < historyLen; ++j ) {
        if ( history[j][0] != '\0' ) {
            fprintf ( fp, "%s\n", history[j] );
        }
    }
    fclose( fp );
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad( const char* filename ) {
    FILE *fp = fopen( filename, "rt" );
    if ( fp == NULL ) {
        return -1;
    }

    char buf[LINENOISE_MAX_LINE];
    while ( fgets( buf, LINENOISE_MAX_LINE, fp ) != NULL ) {
        char* p = strchr( buf, '\r' );
        if ( ! p ) {
            p = strchr( buf, '\n' );
        }
        if ( p ) {
            *p = '\0';
        }
        if ( p != buf ) {
            linenoiseHistoryAdd( buf );
        }
    }
    fclose( fp );
    return 0;
}
