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
#include <stdio.h>
#include <io.h>
#include <errno.h>
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
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cctype>

#endif /* _WIN32 */

#include "linenoise.h"
#include <string>
#include <vector>

using std::string;
using std::vector;

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096

// make control-characters more readable
#define ctrlChar( upperCaseASCII ) ( upperCaseASCII - 0x40 )

struct PromptInfo {                 // a convenience struct for grouping prompt info
    char*   promptText;                 // our copy of the prompt text, edited
    int     promptChars;                // bytes or chars (until UTF-8) in promptText
    int     promptExtraLines;           // extra lines (beyond 1) occupied by prompt
    int     promptIndentation;          // column offset to end of prompt
    int     promptLastLinePosition;     // index into promptText where last line begins
    int     promptPreviousInputLen;     // promptChars of previous input line, for clearing
    int     promptCursorRowOffset;      // where the cursor is relative to the start of the prompt
    int     promptScreenColumns;        // width of screen in columns

    PromptInfo( const char* textPtr, int columns ) : promptScreenColumns( columns ) {

        promptText = new char[strlen( textPtr ) + 1];
        strcpy( promptText, textPtr );

        // strip evil characters from the prompt -- we do allow newline
        unsigned char* pIn = reinterpret_cast<unsigned char *>( promptText );
        unsigned char* pOut = pIn;
        while ( *pIn ) {
            unsigned char c = *pIn;  // we need unsigned so chars 0x80 and above are allowed
            if ( '\n' == c || c >= ' ' ) {
                *pOut = c;
                ++pOut;
            }
            ++pIn;
        }
        *pOut = 0;
        promptChars = pOut - reinterpret_cast<unsigned char *>( promptText );
        promptExtraLines = 0;
        promptLastLinePosition = 0;
        promptPreviousInputLen = 0;
        int x = 0;
        for ( int i = 0; i < promptChars; ++i ) {
            char c = promptText[i];
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
    ~PromptInfo() {
        delete [] promptText;
    }
};

class KillRing {
    vector < string >   theRing;
    int                 index;

public:
    bool lastActionWasKill;

    KillRing() : index( 0 ), lastActionWasKill( false ) {
        theRing.reserve( 1 );
    }

    void kill( const char* text, int textLen, bool forward ) {
        if ( textLen == 0 ) {
            return;
        }
        char* textCopy = new char[ textLen + 1 ];
        memcpy( textCopy, text, textLen );
        textCopy[ textLen ] = 0;
        string textCopyString( textCopy );
        if ( lastActionWasKill ) {
            theRing[index] = forward ?
                theRing[index] + textCopyString :
                textCopyString + theRing[index];
        }
        else {
            theRing.clear();
            theRing.push_back( textCopyString );
        }
        delete[] textCopy;
    }

    string* yank() {
        if ( theRing.size() > 0 && theRing[0].length() > 0 ) {
            return &theRing[0];
        } else {
            return 0;
        }
    }

};

// Special codes for keyboard input:
//
// Between Windows and the various Linux "terminal" programs, there is some
// pretty diverse behavior in the "scan codes" and escape sequences we are
// presented with.  So ... we'll translate them all into our own pidgin
// pseudocode, trying to stay out of the way of UTF-8 and international
// characters.  Here's the general plan.
//
// "User input keystrokes" (key chords, whatever) will be encoded as a single
// value.  The low 8 bits are reserved for ASCII and UTF-8 characters.
// Popular function-type keys get their own codes in the range 0x101 to (if
// needed) 0x1FF, currently just arrow keys, Home, End and Delete.
// Keypresses with Ctrl get or-ed with 0x200, with Alt get or-ed with 0x400.
// So, Ctrl+Alt+Home is encoded as 0x200 + 0x400 + 0x105 == 0x705.  To keep
// things complicated, the Alt key is equivalent to prefixing the keystroke
// with ESC, so ESC followed by D is treated the same as Alt + D ... we'll
// just use Emacs terminology and call this "Meta".  So, we will encode both
// ESC followed by D and Alt held down while D is pressed the same, as Meta-D,
// encoded as 0x464.
//
// Here are the definitions of our component constants:
//
static const int UP_ARROW_KEY       = 0x101;
static const int DOWN_ARROW_KEY     = 0x102;
static const int RIGHT_ARROW_KEY    = 0x103;
static const int LEFT_ARROW_KEY     = 0x104;
static const int HOME_KEY           = 0x105;
static const int END_KEY            = 0x106;
static const int DELETE_KEY         = 0x107;

static const int CTRL               = 0x200;
static const int META               = 0x400;

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
static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
static int history_index = 0;
char** history = NULL;

static void linenoiseAtExit( void );

static void beep() {
    fprintf( stderr, "\x7" );   // ctrl-G == bell/beep
    fflush( stderr );
}

void linenoiseHistoryFree( void ) {
    if ( history ) {
        for ( int j = 0; j < history_len; ++j )
            free( history[j] );
        history_len = 0;
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

static int getColumns( void ) {
    int cols;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO inf;
    GetConsoleScreenBufferInfo( console_out, &inf );
    cols = inf.dwSize.X;
#else
    struct winsize ws;
    cols = ( ioctl( 1, TIOCGWINSZ, &ws ) == -1 ) ? 80 : ws.ws_col;
#endif
    // cols is 0 in certain circumstances like inside debugger, which creates further issues
    return (cols > 0) ? cols : 80;
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
static void calculateScreenPosition(int x, int y, int screenColumns, int charCount, int& xOut, int& yOut) {
    xOut = x;
    yOut = y;
    int charsRemaining = charCount;
    while ( charsRemaining > 0 ) {
        int charsThisRow = (x + charsRemaining < screenColumns) ? charsRemaining : screenColumns - x;
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
 * Refresh the user's input line: the prompt is already onscreen and is not redrawn here
 * @param pi   PromptInfo struct holding information about the prompt and our screen position
 * @param buf  input buffer to be displayed
 * @param len  count of characters in the buffer
 * @param pos  current cursor position within the buffer (0 <= pos <= len)
 */
static void refreshLine( PromptInfo& pi, char *buf, int len, int pos ) {

    // check for a matching brace/bracket/paren, remember its position if found
    int highlight = -1;
    if ( pos < len ) {
        /* this scans for a brace matching buf[pos] to highlight */
        int scanDirection = 0;
        if ( strchr( "}])", buf[pos] ) )
            scanDirection = -1; /* backwards */
        else if ( strchr( "{[(", buf[pos] ) )
            scanDirection = 1; /* forwards */

        if ( scanDirection ) {
            int unmatched = scanDirection;
            for ( int i = pos + scanDirection; i >= 0 && i < len; i += scanDirection ) {
                /* TODO: the right thing when inside a string */
                if ( strchr( "}])", buf[i] ) )
                    unmatched--;
                else if ( strchr( "{[(", buf[i] ) )
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
        if ( write( 1, buf, len ) == -1 ) return;
    }
    else {
        if  (write( 1, buf, highlight ) == -1 ) return;
        setDisplayAttribute( true ); /* bright blue (visible with both B&W bg) */
        if ( write( 1, &buf[highlight], 1 ) == -1 ) return;
        setDisplayAttribute( false );
        if ( write( 1, buf + highlight + 1, len - highlight - 1 ) == -1 ) return;
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
        if ( write( 1, buf, len ) == -1 ) return;
    }
    else {  // highlight the matching brace/bracket/parenthesis
        if ( write( 1, buf, highlight ) == -1 ) return;
        setDisplayAttribute( true );
        if ( write( 1, &buf[highlight], 1 ) == -1 ) return;
        setDisplayAttribute( false );
        if ( write( 1, buf + highlight + 1, len - highlight - 1 ) == -1 ) return;
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
//

// This is a typedef for the routine called by doDispatch().  It takes the current character
// as input, does any required processing including reading more characters and calling other
// dispatch routines, then eventually returns the final (possibly extended or special) character.
//
typedef unsigned int ( *CharacterDispatchRoutine )( unsigned int );

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
static unsigned int doDispatch( unsigned int c, CharacterDispatch& dispatchTable ) {
    for ( unsigned int i = 0; i < dispatchTable.len ; ++i ) {
        if ( static_cast<unsigned char>( dispatchTable.chars[i] ) == c ) {
            return dispatchTable.dispatch[i]( c );
        }
    }
    return dispatchTable.dispatch[dispatchTable.len]( c );
}

static unsigned int thisKeyMetaCtrl = 0;     // holds pre-set Meta and/or Ctrl modifiers

// Final dispatch routines -- return something
//
static unsigned int normalKeyRoutine( unsigned int c )            { return thisKeyMetaCtrl | c; }
static unsigned int upArrowKeyRoutine( unsigned int c )           { return thisKeyMetaCtrl | UP_ARROW_KEY; }
static unsigned int downArrowKeyRoutine( unsigned int c )         { return thisKeyMetaCtrl | DOWN_ARROW_KEY; }
static unsigned int rightArrowKeyRoutine( unsigned int c )        { return thisKeyMetaCtrl | RIGHT_ARROW_KEY; }
static unsigned int leftArrowKeyRoutine( unsigned int c )         { return thisKeyMetaCtrl | LEFT_ARROW_KEY; }
static unsigned int homeKeyRoutine( unsigned int c )              { return thisKeyMetaCtrl | HOME_KEY; }
static unsigned int endKeyRoutine( unsigned int c )               { return thisKeyMetaCtrl | END_KEY; }
static unsigned int deleteCharRoutine( unsigned int c )           { return thisKeyMetaCtrl | ctrlChar( 'H' ); } // key labeled Backspace
static unsigned int deleteKeyRoutine( unsigned int c )            { return thisKeyMetaCtrl | DELETE_KEY; }      // key labeled Delete
static unsigned int ctrlUpArrowKeyRoutine( unsigned int c )       { return thisKeyMetaCtrl | CTRL | UP_ARROW_KEY; }
static unsigned int ctrlDownArrowKeyRoutine( unsigned int c )     { return thisKeyMetaCtrl | CTRL | DOWN_ARROW_KEY; }
static unsigned int ctrlRightArrowKeyRoutine( unsigned int c )    { return thisKeyMetaCtrl | CTRL | RIGHT_ARROW_KEY; }
static unsigned int ctrlLeftArrowKeyRoutine( unsigned int c )     { return thisKeyMetaCtrl | CTRL | LEFT_ARROW_KEY; }
static unsigned int escFailureRoutine( unsigned int c )           { beep(); return -1; }

// Handle ESC [ 1 ; 3 (or 5) <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket1Semicolon3or5Routines[] = { upArrowKeyRoutine, downArrowKeyRoutine, rightArrowKeyRoutine, leftArrowKeyRoutine, escFailureRoutine };
static CharacterDispatch escLeftBracket1Semicolon3or5Dispatch = { 4, "ABCD", escLeftBracket1Semicolon3or5Routines };

// Handle ESC [ 1 ; <more stuff> escape sequences
//
static unsigned int escLeftBracket1Semicolon3Routine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    thisKeyMetaCtrl |= META;
    return doDispatch( c, escLeftBracket1Semicolon3or5Dispatch );
}
static unsigned int escLeftBracket1Semicolon5Routine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    thisKeyMetaCtrl |= CTRL;
    return doDispatch( c, escLeftBracket1Semicolon3or5Dispatch );
}
static CharacterDispatchRoutine escLeftBracket1SemicolonRoutines[] = { escLeftBracket1Semicolon3Routine, escLeftBracket1Semicolon5Routine, escFailureRoutine };
static CharacterDispatch escLeftBracket1SemicolonDispatch = { 2, "35", escLeftBracket1SemicolonRoutines };

// Handle ESC [ 1 <more stuff> escape sequences
//
static unsigned int escLeftBracket1SemicolonRoutine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    return doDispatch( c, escLeftBracket1SemicolonDispatch );
}
static CharacterDispatchRoutine escLeftBracket1Routines[] = { homeKeyRoutine, escLeftBracket1SemicolonRoutine, escFailureRoutine };
static CharacterDispatch escLeftBracket1Dispatch = { 2, "~;", escLeftBracket1Routines };

// Handle ESC [ 3 <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket3Routines[] = { deleteKeyRoutine, escFailureRoutine };
static CharacterDispatch escLeftBracket3Dispatch = { 1, "~", escLeftBracket3Routines };

// Handle ESC [ 4 <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket4Routines[] = { endKeyRoutine, escFailureRoutine };
static CharacterDispatch escLeftBracket4Dispatch = { 1, "~", escLeftBracket4Routines };

// Handle ESC [ 7 <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket7Routines[] = { homeKeyRoutine, escFailureRoutine };
static CharacterDispatch escLeftBracket7Dispatch = { 1, "~", escLeftBracket7Routines };

// Handle ESC [ 8 <more stuff> escape sequences
//
static CharacterDispatchRoutine escLeftBracket8Routines[] = { endKeyRoutine, escFailureRoutine };
static CharacterDispatch escLeftBracket8Dispatch = { 1, "~", escLeftBracket8Routines };

// Handle ESC [ <digit> escape sequences
//
static unsigned int escLeftBracket0Routine( unsigned int c ) {
    return escFailureRoutine( c );
}
static unsigned int escLeftBracket1Routine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    return doDispatch( c, escLeftBracket1Dispatch );
}
static unsigned int escLeftBracket2Routine( unsigned int c ) {
    return escFailureRoutine( c );
}
static unsigned int escLeftBracket3Routine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    return doDispatch( c, escLeftBracket3Dispatch );
}
static unsigned int escLeftBracket4Routine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    return doDispatch( c, escLeftBracket4Dispatch );
}
static unsigned int escLeftBracket5Routine( unsigned int c ) {
    return escFailureRoutine( c );
}
static unsigned int escLeftBracket6Routine( unsigned int c ) {
    return escFailureRoutine( c );
}
static unsigned int escLeftBracket7Routine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    return doDispatch( c, escLeftBracket7Dispatch );
}
static unsigned int escLeftBracket8Routine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    return doDispatch( c, escLeftBracket8Dispatch );
}
static unsigned int escLeftBracket9Routine( unsigned int c ) {
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
static unsigned int escLeftBracketRoutine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    return doDispatch( c, escLeftBracketDispatch );
}
static unsigned int escORoutine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    return doDispatch( c, escODispatch );
}
static unsigned int setMetaRoutine( unsigned int c ); // need forward reference
static CharacterDispatchRoutine escRoutines[] = { escLeftBracketRoutine, escORoutine, setMetaRoutine };
static CharacterDispatch escDispatch = { 2, "[O", escRoutines };

// Initial dispatch -- we are not in the middle of anything yet
//
static unsigned int escRoutine( unsigned int c ) {
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    return doDispatch( c, escDispatch );
}
static unsigned int hibitCRoutine( unsigned int c ) {
    // xterm sends a bizarre sequence for Alt combos: 'C'+0x80 then '!'+0x80 for Alt-a for example
    if ( read( 0, &c, 1 ) <= 0 ) return 0;
    if ( c >= ( ' ' | 0x80 ) && c <= ( '?' | 0x80 ) ) {
        if ( c == ( '?' | 0x80 ) ) {        // have to special case this, normal code would send
            return META | ctrlChar( 'H' );  // Meta-_ (thank you xterm)
        }
        return META | ( c - 0x40 );
    }
    return escFailureRoutine( c );
}
static CharacterDispatchRoutine initialRoutines[] = { escRoutine, deleteCharRoutine, hibitCRoutine, normalKeyRoutine };
static CharacterDispatch initialDispatch = { 3, "\x1B\x7F\xC3", initialRoutines };

// Special handling for the ESC key because it does double duty
//
static unsigned int setMetaRoutine( unsigned int c ) {
    thisKeyMetaCtrl = META;
    if ( c == 0x1B ) {  // another ESC, stay in ESC processing mode
        if ( read( 0, &c, 1 ) <= 0 ) return 0;
        return doDispatch( c, escDispatch );
    }
    return doDispatch( c, initialDispatch );
}
#endif // #ifndef _WIN32

// linenoiseReadChar -- read a keystroke or keychord from the keyboard, and translate it
// into an encoded "keystroke".  When convenient, extended keys are translated into their
// simpler Emacs keystrokes, so an unmodified "left arrow" becomes Ctrl-B.
//
// A return value of zero means "no input available", and a return value of -1 means "invalid key".
//
static int linenoiseReadChar( void ){
#ifdef _WIN32
    INPUT_RECORD rec;
    DWORD count;
    int modifierKeys = 0;
    bool escSeen = false;
    while ( true ) {
        ReadConsoleInputA( console_in, &rec, 1, &count );
        if ( rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown ) {
            continue;
        }
        modifierKeys = 0;
        if ( rec.Event.KeyEvent.dwControlKeyState & ( RIGHT_CTRL_PRESSED | LEFT_CTRL_PRESSED ) ) {
            modifierKeys |= CTRL;
        }
        if ( rec.Event.KeyEvent.dwControlKeyState & ( RIGHT_ALT_PRESSED | LEFT_ALT_PRESSED ) ) {
            modifierKeys |= META;
        }
        if ( escSeen ) {
            modifierKeys |= META;
        }
        if ( rec.Event.KeyEvent.uChar.AsciiChar == 0 ) {
            switch ( rec.Event.KeyEvent.wVirtualKeyCode ) {
                case VK_LEFT:   return modifierKeys | LEFT_ARROW_KEY;
                case VK_RIGHT:  return modifierKeys | RIGHT_ARROW_KEY;
                case VK_UP:     return modifierKeys | UP_ARROW_KEY;
                case VK_DOWN:   return modifierKeys | DOWN_ARROW_KEY;
                case VK_DELETE: return modifierKeys | DELETE_KEY;
                case VK_HOME:   return modifierKeys | HOME_KEY;
                case VK_END:    return modifierKeys | END_KEY;
                default: continue;                      // in raw mode, ReadConsoleInput shows shift, ctrl ...
            }                                           //  ... ignore them
        }
        else if ( rec.Event.KeyEvent.uChar.AsciiChar == ctrlChar( '[' ) ) { // ESC, set flag for later
            escSeen = true;
            continue;
        }
        else {
            // we got a real character, return it
            return modifierKeys | rec.Event.KeyEvent.uChar.AsciiChar;
        }
    }
#else
    unsigned int c;
    unsigned char oneChar;
    int nread;

    nread = read( 0, &oneChar, 1 );
    if ( nread <= 0 )
        return 0;
    c = static_cast<unsigned int>( oneChar );

    // If _DEBUG_LINUX_KEYBOARD is set, then ctrl-\ puts us into a keyboard debugging mode
    // where we print out decimal and decoded values for whatever the "terminal" program
    // gives us on different keystrokes.  Hit ctrl-C to exit this mode.
    //
#define _DEBUG_LINUX_KEYBOARD
#if defined(_DEBUG_LINUX_KEYBOARD)
    if ( c == 28 ) {    // ctrl-\, special debug mode, prints all keys hit, ctrl-C to get out.
        printf( "\x1b[1G\n" ); /* go to first column of new line */
        while ( true ) {
            unsigned char keys[10];
            int ret = read( 0, keys, 10 );

            if ( ret <= 0 ) {
                printf( "\nret: %d\n", ret );
            }
            for ( int i = 0; i < ret; ++i ) {
                unsigned int key = static_cast<unsigned int>( keys[i] );
                char * friendlyTextPtr;
                char friendlyTextBuf[10];
                const char * prefixText = (key < 0x80) ? "" : "highbit-";
                unsigned int keyCopy = (key < 0x80) ? key : key - 0x80;
                if ( keyCopy >= '!' && keyCopy <= '~' ) {   // printable
                    friendlyTextBuf[0] = '\'';
                    friendlyTextBuf[1] = keyCopy;
                    friendlyTextBuf[2] = '\'';
                    friendlyTextBuf[3] = 0;
                    friendlyTextPtr = friendlyTextBuf;
                }
                else if ( keyCopy == ' ' ) {
                    friendlyTextPtr = (char *)"space";
                }
                else if (keyCopy == 27 ) {
                    friendlyTextPtr = (char *)"ESC";
                }
                else if (keyCopy == 0 ) {
                    friendlyTextPtr = (char *)"NUL";
                }
                else if (keyCopy == 127 ) {
                    friendlyTextPtr = (char *)"DEL";
                }
                else {
                    friendlyTextBuf[0] = '^';
                    friendlyTextBuf[1] = keyCopy + 0x40;
                    friendlyTextBuf[2] = 0;
                    friendlyTextPtr = friendlyTextBuf;
                }
                printf( "%d (%s%s)  ", key, prefixText, friendlyTextPtr );
            }
            printf( "\x1b[1G\n" );  // go to first column of new line

            // drop out of this loop on ctrl-C
            if ( keys[0] == ctrlChar( 'C' ) ) {
                return -1;
            }
        }
    }
#endif  // _DEBUG_LINUX_KEYBOARD

    thisKeyMetaCtrl = 0;    // no modifiers yet at initialDispatch
    return doDispatch( c, initialDispatch );
#endif // #_WIN32
}

static void freeCompletions( linenoiseCompletions* lc ) {
    for ( size_t i = 0; i < lc->len; ++i )
        free( lc->cvec[i] );
    if ( lc->cvec )
        free( lc->cvec );
}

// break characters that may precede items to be completed
static const char breakChars[] = " =+-/\\*?\"'`&<>;|@{([])}";

/**
 * Handle command completion, using a completionCallback() routine to provide possible substitutions
 * This routine handles the mechanics of updating the user's input buffer with possible replacement of
 * text as the user selects a proposed completion string, or cancels the completion attempt.
 * @param pi     PromptInfo struct holding information about the prompt and our screen position
 * @param buf    input buffer to be displayed
 * @param buflen size of input buffer in bytes
 * @param len    ptr to count of characters in the buffer (updated)
 * @param pos    ptr to current cursor position within the buffer (0 <= pos <= len) (updated)
 */
static int completeLine( PromptInfo& pi, char *buf, int buflen, int *len, int *pos ) {
    linenoiseCompletions lc = { 0, NULL };
    char c = 0;

    // completionCallback() expects a parsable entity, so find the previous break character and extract
    // a copy to parse.  we also handle the case where tab is hit while not at end-of-line.
    int startIndex = *pos;
    while ( --startIndex >= 0 ) {
        if ( strchr( breakChars, buf[startIndex] ) ) {
            break;
        }
    }
    ++startIndex;
    int itemLength = *pos - startIndex;
    char* parseItem = reinterpret_cast<char *>( malloc( itemLength + 1 ) );
    int i = 0;
    for ( ; i < itemLength; ++i ) {
        parseItem[i] = buf[startIndex+i];
    }
    parseItem[i] = 0;

    // get a list of completions
    completionCallback( parseItem, &lc );
    free( parseItem );
    int displayLength = 0;
    char * displayText = 0;
    if ( lc.len == 0 ) {
        beep();
    }
    else {
        size_t i = 0;
        size_t clen;

        bool stop = false;
        while ( ! stop ) {
            /* Show completion or original buffer */
            if ( i < lc.len ) {
                clen = strlen( lc.cvec[i] );
                displayLength = *len + clen - itemLength;
                displayText = reinterpret_cast<char *>( malloc( displayLength + 1 ) );
                int j = 0;
                for ( ; j < startIndex; ++j )
                    displayText[j] = buf[j];
                strcpy( &displayText[j], lc.cvec[i] );
                strcpy( &displayText[j+clen], &buf[*pos] );
                displayText[displayLength] = 0;
                refreshLine( pi, displayText, displayLength, startIndex + clen );
                free( displayText );
            }
            else {
                refreshLine( pi, buf, *len, *pos );
            }

            do {
                c = linenoiseReadChar();
            } while ( c == static_cast<char>( -1 ) );

            switch ( c ) {

                case 0:
                    freeCompletions( &lc );
                    return -1;

                case ctrlChar( 'I' ):   // ctrl-I/tab
                    i = ( i + 1 ) % ( lc.len + 1 );
                    if ( i == lc.len )
                        beep();         // beep after completing cycle
                    break;

#if 0 // SERVER-4011 -- Escape only works to end command completion in Windows
      // leaving code here for now in case this is where we will add Meta-R (revert-line) later
                case 27: /* escape */
                    /* Re-show original buffer */
                    if ( i < lc.len )
                        refreshLine( pi, buf, *len, *pos );
                    stop = true;
                    break;
#endif // SERVER-4011 -- Escape only works to end command completion in Windows

                default:
                    /* Update buffer and return */
                    if ( i < lc.len ) {
                        clen = strlen( lc.cvec[i] );
                        displayLength = *len + clen - itemLength;
                        displayText = (char *)malloc( displayLength + 1 );
                        int j = 0;
                        for ( ; j < startIndex; ++j )
                            displayText[j] = buf[j];
                        strcpy( &displayText[j], lc.cvec[i] );
                        strcpy( &displayText[j+clen], &buf[*pos] );
                        displayText[displayLength] = 0;
                        strcpy( buf, displayText );
                        free( displayText );
                        *pos = startIndex + clen;
                        *len = displayLength;
                    }
                    stop = true;
                    break;
            }
        }
    }

    freeCompletions( &lc );
    return c; /* Return last read character */
}

static void linenoiseClearScreen( PromptInfo& pi, char *buf, int len, int pos ) {

#ifdef _WIN32
    COORD coord = {0, 0};
    CONSOLE_SCREEN_BUFFER_INFO inf;
    GetConsoleScreenBufferInfo( console_out, &inf );
    SetConsoleCursorPosition( console_out, coord );
    DWORD count;
    FillConsoleOutputCharacterA( console_out, ' ', inf.dwSize.X * inf.dwSize.Y, coord, &count );
#else
    if ( write( 1, "\x1b[H\x1b[2J", 7 ) <= 0 ) return;
#endif
    if ( write( 1, pi.promptText, pi.promptChars ) == -1 ) return;
    pi.promptCursorRowOffset = pi.promptExtraLines;
    refreshLine( pi, buf, len, pos );
}

// convert {CTRL + 'A'}, {CTRL + 'a'} and {CTRL + ctrlChar( 'A' )} into ctrlChar( 'A' )
// leave META alone
//
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

static int linenoisePrompt( char *buf, int buflen, PromptInfo& pi ) {
    int pos = 0;
    int len = 0;

    buf[0] = '\0';
    buflen--; /* Make sure there is always space for the nulterm */

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    linenoiseHistoryAdd( "" );
    history_index = history_len-1;

    // display the prompt
    if ( write( 1, pi.promptText, pi.promptChars ) == -1 ) return -1;

    // the cursor starts out at the end of the prompt
    pi.promptCursorRowOffset = pi.promptExtraLines;

    // new prompt resets 'lastActionWasKill' state
    killRing.lastActionWasKill = false;

    // loop collecting characters, responding to ctrl characters
    while ( true ) {
        int c = linenoiseReadChar();
        c = cleanupCtrl( c );

        if ( c == 0 )
            return len;

        if ( c == -1 ) {
            refreshLine( pi, buf, len, pos );
            continue;
        }

        // ctrl-I/tab, command completion, needs to be before switch statement
        if ( c == ctrlChar( 'I' ) && completionCallback ) {
            killRing.lastActionWasKill = false;

            // completeLine does the actual completion and replacement
            c = completeLine( pi, buf, buflen, &len, &pos );

            if ( c < 0 )                // return on error
                return len;

            if ( c == 0 )               // read next character when 0
                continue;

            // deliberate fall-through here, so we use the terminating character
        }

        switch ( c ) {

        case ctrlChar( 'A' ):   // ctrl-A, move cursor to start of line
        case HOME_KEY:
            killRing.lastActionWasKill = false;
            pos = 0;
            refreshLine( pi ,buf, len, pos );
            break;

        case ctrlChar( 'B' ):   // ctrl-B, move cursor left by one character
        case LEFT_ARROW_KEY:
            killRing.lastActionWasKill = false;
            if ( pos > 0 ) {
                --pos;
                refreshLine( pi, buf, len, pos );
            }
            break;

        case META + 'b':        // meta-B, move cursor left by one word
        case META + 'B':
        case CTRL + LEFT_ARROW_KEY:
        case META + LEFT_ARROW_KEY: // Emacs allows Meta, bash & readline don't
            killRing.lastActionWasKill = false;
            if( pos > 0 ) {
                while ( pos > 0 && !isalnum( buf[pos - 1] ) ) {
                    --pos;
                }
                while ( pos > 0 && isalnum( buf[pos - 1] ) ) {
                    --pos;
                }
                refreshLine( pi, buf, len, pos );
            }
            break;

        case ctrlChar( 'C' ):   // ctrl-C, abort this line
            killRing.lastActionWasKill = false;
            errno = EAGAIN;
            --history_len;
            free( history[history_len] );
            // we need one last refresh with the cursor at the end of the line
            // so we don't display the next prompt over the previous input line
            refreshLine( pi, buf, len, len );  // pass len as pos for EOL
            if ( write( 1, "^C", 2 ) == -1 ) return -1;    // Display the ^C we got
            return -1;

        // ctrl-D, delete the character under the cursor
        // on an empty line, exit the shell
        case ctrlChar( 'D' ):
            killRing.lastActionWasKill = false;
            if( len > 0 && pos < len ) {
                memmove( buf + pos, buf + pos + 1, len - pos );
                --len;
                refreshLine( pi, buf, len, pos );
            }
            else if( len == 0 ) {
                history_len--;
                free( history[history_len] );
                return -1;
            }
            break;

        case META + 'd':        // meta-D, kill word to right of cursor
        case META + 'D':
            if( pos < len ) {
                int endingPos = pos;
                while ( endingPos < len && !isalnum( buf[endingPos] ) ) {
                    ++endingPos;
                }
                while ( endingPos < len && isalnum( buf[endingPos] ) ) {
                    ++endingPos;
                }
                killRing.kill( &buf[pos], endingPos - pos, true );
                memmove( buf + pos, buf + endingPos, len - endingPos + 1 );
                len -= endingPos - pos;
                refreshLine( pi, buf, len, pos );
            }
            killRing.lastActionWasKill = true;
            break;

        case ctrlChar( 'E' ):   // ctrl-E, move cursor to end of line
        case END_KEY:
            killRing.lastActionWasKill = false;
            pos = len;
            refreshLine( pi, buf, len, pos );
            break;

        case ctrlChar( 'F' ):   // ctrl-F, move cursor right by one character
        case RIGHT_ARROW_KEY:
            killRing.lastActionWasKill = false;
            if ( pos < len ) {
                ++pos;
                refreshLine( pi ,buf, len, pos );
            }
            break;

        case META + 'f':        // meta-F, move cursor right by one word
        case META + 'F':
        case CTRL + RIGHT_ARROW_KEY:
        case META + RIGHT_ARROW_KEY: // Emacs allows Meta, bash & readline don't
            killRing.lastActionWasKill = false;
            if( pos < len ) {
                while ( pos < len && !isalnum( buf[pos] ) ) {
                    ++pos;
                }
                while ( pos < len && isalnum( buf[pos] ) ) {
                    ++pos;
                }
                refreshLine( pi, buf, len, pos );
            }
            break;

        case ctrlChar( 'H' ):   // backspace/ctrl-H, delete char to left of cursor
            killRing.lastActionWasKill = false;
            if ( pos > 0 ) {
                memmove( buf + pos - 1, buf + pos, 1 + len - pos );
                --pos;
                --len;
                refreshLine( pi, buf, len, pos );
            }
            break;

        // meta-Backspace, kill word to left of cursor
        case META + ctrlChar( 'H' ):
            if( pos > 0 ) {
                int startingPos = pos;
                while ( pos > 0 && !isalnum( buf[pos - 1] ) ) {
                    --pos;
                }
                while ( pos > 0 && isalnum( buf[pos - 1] ) ) {
                    --pos;
                }
                killRing.kill( &buf[pos], startingPos - pos, false );
                memmove( buf + pos, buf + startingPos, len - startingPos + 1 );
                len -= startingPos - pos;
                refreshLine( pi, buf, len, pos );
            }
            killRing.lastActionWasKill = true;
            break;

        case ctrlChar( 'J' ):   // ctrl-J/linefeed/newline, accept line
        case ctrlChar( 'M' ):   // ctrl-M/return/enter
            killRing.lastActionWasKill = false;
            // we need one last refresh with the cursor at the end of the line
            // so we don't display the next prompt over the previous input line
            refreshLine( pi, buf, len, len );  // pass len as pos for EOL
            --history_len;
            free( history[history_len] );
            return len;

        case ctrlChar( 'K' ):   // ctrl-K, kill from cursor to end of line
            killRing.kill( &buf[pos], len - pos, true );
            buf[pos] = '\0';
            len = pos;
            refreshLine( pi, buf, len, pos );
            killRing.lastActionWasKill = true;
            break;

        case ctrlChar( 'L' ):   // ctrl-L, clear screen and redisplay line
            linenoiseClearScreen( pi, buf, len, pos );
            break;

        case ctrlChar( 'N' ):   // ctrl-N, recall next line in history
        case ctrlChar( 'P' ):   // ctrl-P, recall previous line in history
        case DOWN_ARROW_KEY:
        case UP_ARROW_KEY:
            killRing.lastActionWasKill = false;
            if ( history_len > 1 ) {
                /* Update the current history entry before we
                 * overwrite it with the next one. */
                free( history[history_index] );
                history[history_index] = strdup (buf );
                /* Show the new entry */
                if ( c == UP_ARROW_KEY ) {
                    c = ctrlChar( 'P' );
                }
                history_index += ( c == ctrlChar( 'P' ) ) ? -1 : 1;
                if ( history_index < 0 ) {
                    history_index = 0;
                    break;
                }
                else if ( history_index >= history_len ) {
                    history_index = history_len - 1;
                    break;
                }
                strncpy( buf, history[history_index], buflen );
                buf[buflen] = '\0';
                len = pos = strlen( buf );  // place cursor at end of line
                refreshLine( pi, buf, len, pos );
            }
            break;

        case ctrlChar( 'T' ):   // ctrl-T, transpose characters
            killRing.lastActionWasKill = false;
            if ( pos > 0 && len > 1 ) {
                size_t leftCharPos = ( pos == len ) ? pos - 2 : pos - 1;
                char aux = buf[leftCharPos];
                buf[leftCharPos] = buf[leftCharPos+1];
                buf[leftCharPos+1] = aux;
                if ( pos != len )
                    ++pos;
                refreshLine( pi ,buf, len, pos );
            }
            break;

        case ctrlChar( 'U' ):   // ctrl-U, kill all characters to the left of the cursor
            if( pos > 0 ) {
                killRing.kill( &buf[0], pos, false );
                len -= pos;
                memmove( buf, buf + pos, len + 1 );
                pos = 0;
                refreshLine( pi, buf, len, pos );
            }
            killRing.lastActionWasKill = true;
            break;

        // ctrl-W, kill to whitespace (not word) to left of cursor
        case ctrlChar( 'W' ):
            if( pos > 0 ) {
                int startingPos = pos;
                while ( pos > 0 && buf[pos - 1] == ' ' ) {
                    --pos;
                }
                while ( pos > 0 && buf[pos - 1] != ' ' ) {
                    --pos;
                }
                killRing.kill( &buf[pos], startingPos - pos, false );
                memmove( buf + pos, buf + startingPos, len - startingPos + 1 );
                len -= startingPos - pos;
                refreshLine( pi, buf, len, pos );
            }
            killRing.lastActionWasKill = true;
            break;

        case ctrlChar( 'Y' ):   // ctrl-Y, yank killed text
            killRing.lastActionWasKill = false;
            {
                string* restoredText = killRing.yank();
                if ( restoredText ) {
                    int restoredTextLen = restoredText->length();
                    memmove( buf + pos + restoredTextLen, buf + pos, len - pos + 1 );
                    memmove( buf + pos, restoredText->c_str(), restoredTextLen );
                    pos += restoredTextLen;
                    len += restoredTextLen;
                    refreshLine( pi, buf, len, pos );
                }
                else {
                    beep();
                }
            }
            break;

#if 0 // beep until implemented properly
        case META + 'y':        // meta-Y, "yank-pop", rotate popped text
        case META + 'Y':
            killRing.lastActionWasKill = false;
            {
                string restoredText = killRing.yankPop();
                int restoredTextLen = restoredText.length();
                memmove( buf + pos + restoredTextLen, buf + pos, len - pos + 1 );
                memmove( buf + pos, restoredText.c_str(), restoredTextLen );
                pos += restoredTextLen;
                len += restoredTextLen;
                refreshLine( pi, buf, len, pos );
            }
            break;
#endif

#ifndef _WIN32
        case ctrlChar( 'Z' ):   // ctrl-Z, job control
            disableRawMode();                       // Returning to Linux (whatever) shell, leave raw mode
            raise( SIGSTOP );                       // Break out in mid-line
            enableRawMode();                        // Back from Linux shell, re-enter raw mode
            if ( write( 1, pi.promptText, pi.promptChars ) == -1 ) break; // Redraw prompt
            refreshLine( pi, buf, len, pos );   // Refresh the line
            break;
#endif

        // DEL, delete the character under the cursor
        case 127:
        case DELETE_KEY:
            killRing.lastActionWasKill = false;
            if( len > 0 && pos < len ) {
                memmove( buf + pos, buf + pos + 1, len - pos );
                --len;
                refreshLine( pi, buf, len, pos );
            }
            break;

        // not one of our special characters, maybe insert it in the buffer
        default:
            killRing.lastActionWasKill = false;
            if ( c > 0xFF ) {   // beep on unknown Ctrl and/or Meta keys
                beep();
                break;
            }
            if ( len < buflen ) {
                if ( static_cast<unsigned char>( c ) < 32 ) {   // don't insert control characters
                    beep();
                    break;
                }
                if ( len == pos ) {     // at end of buffer
                    buf[pos] = c;
                    ++pos;
                    ++len;
                    buf[len] = '\0';
                    if ( pi.promptIndentation + len < pi.promptScreenColumns ) {
                        if ( len > pi.promptPreviousInputLen )
                            pi.promptPreviousInputLen = len;
                        /* Avoid a full update of the line in the
                         * trivial case. */
                        if ( write( 1, &c, 1) == -1 ) return -1;
                    }
                    else {
                        refreshLine( pi, buf, len, pos );
                    }
                }
                else {  // not at end of buffer, have to move characters to our right
                    memmove( buf + pos + 1, buf + pos, len - pos );
                    buf[pos] = c;
                    ++len;
                    ++pos;
                    buf[len] = '\0';
                    refreshLine( pi, buf, len, pos );
                }
            }
            break;
        }
    }
    return len;
}

static int linenoiseRaw( char* buf, int buflen, PromptInfo& pi ) {
    int count;

    if ( buflen == 0 ) {
        errno = EINVAL;
        return -1;
    }

    if ( isatty( STDIN_FILENO ) ) {     // input is from a terminal
        if ( enableRawMode() == -1 )
            return -1;
        count = linenoisePrompt( buf, buflen, pi );
        disableRawMode();
        printf( "\n" );
    }
    else {  // input not from a terminal, we should work with piped input, i.e. redirected stdin
        if ( fgets( buf, buflen, stdin ) == NULL )
            return -1;
        count = strlen( buf );

        // if fgets() gave us the newline, remove it
        if ( count && buf[count-1] == '\n' ) {
            --count;
            buf[count] = '\0';
        }
    }
    return count;
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
    char buf[LINENOISE_MAX_LINE];               // buffer for user's input
    PromptInfo pi( prompt, getColumns() );      // struct to hold edited copy of prompt & misc prompt info
    if ( linenoiseRaw( buf, LINENOISE_MAX_LINE, pi ) == -1 ) {
        return NULL;
    }
    return strdup( buf );                       // caller must free buffer
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback( linenoiseCompletionCallback* fn ) {
    completionCallback = fn;
}

void linenoiseAddCompletion( linenoiseCompletions* lc, const char* str ) {
    size_t len = strlen( str );
    char* copy = reinterpret_cast<char *>( malloc( len + 1 ) );
    memcpy( copy, str, len + 1 );
    lc->cvec = reinterpret_cast<char**>( realloc( lc->cvec, sizeof( char* ) * ( lc->len + 1 ) ) );
    lc->cvec[lc->len++] = copy;
}

int linenoiseHistoryAdd( const char* line ) {
    if ( history_max_len == 0 )
        return 0;
    if ( history == NULL ) {
        history = reinterpret_cast<char**>( malloc( sizeof( char* ) * history_max_len ) );
        if (history == NULL)
            return 0;
        memset( history, 0, (sizeof(char*) * history_max_len ) );
    }
    char* linecopy = strdup( line );
    if ( ! linecopy )
        return 0;
    if ( history_len == history_max_len ) {
        free( history[0] );
        memmove( history, history + 1, sizeof(char*) * ( history_max_len - 1 ) );
        --history_len;
    }

    // convert newlines in multi-line code to spaces before storing
    char *p = linecopy;
    while( *p ) {
        if( *p == '\n' )
            *p = ' ';
        ++p;
    }
    history[history_len] = linecopy;
    ++history_len;
    return 1;
}

int linenoiseHistorySetMaxLen( int len ) {
    if ( len < 1 )
        return 0;
    if ( history ) {
        int tocopy = history_len;
        char** newHistory = reinterpret_cast<char**>( malloc( sizeof(char*) * len ) );
        if ( newHistory == NULL )
            return 0;
        if ( len < tocopy )
            tocopy = len;
        memcpy( newHistory, history + history_max_len - tocopy, sizeof(char*) * tocopy );
        free( history );
        history = newHistory;
    }
    history_max_len = len;
    if ( history_len > history_max_len )
        history_len = history_max_len;
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave( const char* filename ) {
    FILE* fp = fopen( filename, "wt" );
    if ( fp == NULL )
        return -1;
    for ( int j = 0; j < history_len; ++j ) {
        if ( history[j][0] != '\0' )
            fprintf (fp, "%s\n", history[j] );
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
    char buf[LINENOISE_MAX_LINE];
    
    if ( fp == NULL )
        return -1;

    while ( fgets( buf, LINENOISE_MAX_LINE, fp ) != NULL ) {
        char* p = strchr( buf, '\r' );
        if ( ! p )
            p = strchr( buf, '\n' );
        if ( p )
            *p = '\0';
        if ( p != buf )
            linenoiseHistoryAdd( buf );
    }
    fclose( fp );
    return 0;
}
