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

#include <boost/scoped_array.hpp>

#ifdef _WIN32

#include <conio.h>
#include <windows.h>
#include <stdio.h>
#include <io.h>
#include <errno.h>
#define snprintf _snprintf
#define strcasecmp _stricmp
#define strdup _strdup
#define isatty _isatty
#define write _write
#define STDIN_FILENO 0

#else /* _WIN32 */

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

#endif /* _WIN32 */

#include "linenoise.h"

    typedef struct tag_PROMPTINFO {
        char *  promptText;
        int     promptChars;
        int     promptExtraLines;
        int     promptIndentation;
        int     promptLastLinePosition;
        int     promptPreviousInputLen;
        int     promptCursorRowOffset;
    } PROMPTINFO;

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096

// make control-characters more readable
#define ctrlChar(upperCaseASCII) (upperCaseASCII - 0x40)

static const char *unsupported_term[] = {"dumb","cons25",NULL};
static linenoiseCompletionCallback *completionCallback = NULL;

#ifdef _WIN32
static HANDLE console_in, console_out;
static DWORD oldMode;
#else
static struct termios orig_termios; /* in order to restore at exit */
#endif

static int rawmode = 0; /* for atexit() function to check if restore is needed*/
static int atexit_registered = 0; /* register atexit just 1 time */
static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
static int history_index = 0;
char **history = NULL;

static void linenoiseAtExit(void);
int linenoiseHistoryAdd(const char *line);

static int isUnsupportedTerm(void) {
    char *term = getenv("TERM");
    int j;

    if (term == NULL) return 0;
    for (j = 0; unsupported_term[j]; j++)
        if (!strcasecmp(term,unsupported_term[j])) return 1;
    return 0;
}

void linenoiseHistoryFree(void) {
    if (history) {
        int j;

        for (j = 0; j < history_len; j++)
            free(history[j]);
        free(history);
    }
}

static int enableRawMode(int fd) {
#ifdef _WIN32
    if (!console_in) {
        console_in = GetStdHandle(STD_INPUT_HANDLE);
        console_out = GetStdHandle(STD_OUTPUT_HANDLE);

        GetConsoleMode(console_in, &oldMode);
        SetConsoleMode(console_in, oldMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
    }
    return 0;
#else
    struct termios raw;

    if (!isatty(STDIN_FILENO)) goto fatal;
    if (!atexit_registered) {
        atexit(linenoiseAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    // this is wrong, we don't want raw output, it turns newlines into straight linefeeds
    //raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - echoing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSADRAIN,&raw) < 0) goto fatal;
    rawmode = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
#endif
}

static void disableRawMode(int fd) {
#ifdef _WIN32
    SetConsoleMode(console_in, oldMode);
    console_in = 0;
    console_out = 0;
#else
    /* Don't even check the return value as it's too late. */
    if (rawmode && tcsetattr(fd,TCSADRAIN,&orig_termios) != -1)
        rawmode = 0;
#endif
}

/* At exit we'll try to fix the terminal to the initial conditions. */
static void linenoiseAtExit(void) {
    disableRawMode(STDIN_FILENO);
}

static int getColumns(void) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO inf = { 0 };
    GetConsoleScreenBufferInfo(console_out, &inf);
    return inf.dwSize.X;
#else
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1) return 80;
    return ws.ws_col;
#endif
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
        y++;
    }
    if ( xOut == screenColumns ) {  // we have to special-case line wrap
        xOut = 0;
        yOut++;
    }
}

/**
 * Refresh the user's input line: the prompt is already onscreen and is not redrawn here
 * @param fd   file handle to use for output to the screen
 * @param pi   PROMPTINFO struct holding information about the prompt and our screen position
 * @param buf  input buffer to be displayed
 * @param len  count of characters in the buffer
 * @param pos  current cursor position within the buffer (0 <= pos <= len)
 * @param cols screen width in columns
 */
static void refreshLine(int fd, PROMPTINFO & pi, char *buf, int len, int pos, int cols) {

    // check for a matching brace/bracket/paren, remember its position if found
    int highlight = -1;
    if ( pos < len ) {
        /* this scans for a brace matching buf[pos] to highlight */
        int scanDirection = 0;
        if ( strchr("}])", buf[pos]) )
            scanDirection = -1; /* backwards */
        else if ( strchr("{[(", buf[pos]) )
            scanDirection = 1; /* forwards */

        if ( scanDirection ) {
            int unmatched = scanDirection;
            for ( int i = pos + scanDirection; i >= 0 && i < len; i += scanDirection ) {
                /* TODO: the right thing when inside a string */
                if ( strchr("}])", buf[i]) )
                    unmatched--;
                else if ( strchr("{[(", buf[i]) )
                    unmatched++;

                if ( unmatched == 0 ) {
                    highlight = i;
                    break;
                }
            }
        }
    }

    // calculate the position of the end of the input line
    int xEndOfInput, yEndOfInput;
    calculateScreenPosition(pi.promptIndentation, 0, cols, len, xEndOfInput, yEndOfInput);

    // calculate the desired position of the cursor
    int xCursorPos, yCursorPos;
    calculateScreenPosition(pi.promptIndentation, 0, cols, pos, xCursorPos, yCursorPos);

#ifdef _WIN32
    // position at the end of the prompt, clear to end of previous input
    CONSOLE_SCREEN_BUFFER_INFO inf;
    GetConsoleScreenBufferInfo(console_out, &inf);
    inf.dwCursorPosition.X = pi.promptIndentation;  // 0-based on Win32
    inf.dwCursorPosition.Y -= pi.promptCursorRowOffset - pi.promptExtraLines;
    SetConsoleCursorPosition(console_out, inf.dwCursorPosition);
    DWORD count;
    if ( len < pi.promptPreviousInputLen )
        FillConsoleOutputCharacterA(console_out, ' ', pi.promptPreviousInputLen, inf.dwCursorPosition, &count);
    pi.promptPreviousInputLen = len;

    // display the input line
    if ( write(1,buf,len) == -1 ) return;

    // position the cursor
    GetConsoleScreenBufferInfo(console_out, &inf);
    inf.dwCursorPosition.X = xCursorPos;  // 0-based on Win32
    inf.dwCursorPosition.Y -= yEndOfInput - yCursorPos;
    SetConsoleCursorPosition(console_out, inf.dwCursorPosition);
#else // _WIN32
    char seq[64];
    int cursorRowMovement = pi.promptCursorRowOffset - pi.promptExtraLines;
    if ( cursorRowMovement > 0 ) {  // move the cursor up as required
        snprintf(seq, sizeof seq, "\x1b[%dA", cursorRowMovement);
        if ( write(fd,seq,strlen(seq)) == -1 ) return;
    }
    // position at the end of the prompt, clear to end of screen
    snprintf(seq, sizeof seq, "\x1b[%dG\x1b[J", pi.promptIndentation + 1);  // 1-based on VT100
    if ( write(fd,seq,strlen(seq)) == -1 ) return;

    if ( highlight == -1 ) {  // write unhighlighted text
        if ( write(fd,buf,len) == -1 ) return;
    }
    else {  // highlight the matching brace/bracket/parenthesis
        if ( write(fd, buf, highlight) == -1 ) return;
        if ( write(fd, "\x1b[1;34m", 7) == -1 ) return; /* bright blue (visible with both B&W bg) */
        if ( write(fd, &buf[highlight], 1) == -1 ) return;
        if ( write(fd, "\x1b[0m", 4) == -1 ) return; /* reset */
        if ( write(fd, buf+highlight+1, len-highlight-1) == -1 ) return;
    }

    // we have to generate our own newline on line wrap
    if ( xEndOfInput == 0 && yEndOfInput > 0 )
        if ( write( fd, "\n", 1 ) == -1 ) return;

    // position the cursor
    cursorRowMovement = yEndOfInput - yCursorPos;
    if ( cursorRowMovement > 0 ) {  // move the cursor up as required
        snprintf(seq, sizeof seq, "\x1b[%dA", cursorRowMovement);
        if ( write(fd,seq,strlen(seq)) == -1 ) return;
    }
    // position the cursor within the line
    snprintf(seq, sizeof seq, "\x1b[%dG", xCursorPos + 1);  // 1-based on VT100
    if ( write(fd,seq,strlen(seq)) == -1 ) return;
#endif

    pi.promptCursorRowOffset = pi.promptExtraLines + yCursorPos;  // remember row for next pass
}

/* Note that this should parse some special keys into their emacs ctrl-key combos
 * Return of -1 signifies unrecognized code
 */
static char linenoiseReadChar(int fd){
#ifdef _WIN32
    INPUT_RECORD rec;
    DWORD count;
    do {
        ReadConsoleInputA(console_in, &rec, 1, &count);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) {
            continue;
        }
        if (rec.Event.KeyEvent.uChar.AsciiChar == 0) {
            /* handle keys that aren't converted to ASCII */
            switch (rec.Event.KeyEvent.wVirtualKeyCode) {
                case VK_LEFT:   return ctrlChar('B');  // EMACS character (B)ack
                case VK_RIGHT:  return ctrlChar('F');  // EMACS character (F)orward
                case VK_UP:     return ctrlChar('P');  // EMACS (P)revious line
                case VK_DOWN:   return ctrlChar('N');  // EMACS (N)ext line
                case VK_DELETE: return 127;            // ASCII DEL byte
                case VK_HOME:   return ctrlChar('A');  // EMACS beginning-of-line
                case VK_END:    return ctrlChar('E');  // EMACS (E)nd-of-line
                default: continue;                     // in raw mode, ReadConsoleInput shows shift, ctrl ...
            }                                          //  ... ignore them
        }
        else {
            break;  // we got a real character, return it
        }
    } while (true);
    return rec.Event.KeyEvent.uChar.AsciiChar;
#else
    char c;
    int nread;
    char seq[2], seq2[2];

    nread = read(fd,&c,1);
    if (nread <= 0) return 0;

#if defined(_DEBUG)
    if (c == 28) { /* ctrl-\ */
        /* special debug mode. prints all keys hit. ctrl-c to get out */
        printf("\x1b[1G\n"); /* go to first column of new line */
        while (true) {
            char keys[10];
            int ret = read(fd, keys, 10);
            int i;

            if (ret <= 0) {
                printf("\nret: %d\n", ret);
            }

            for (i=0; i < ret; i++)
                printf("%d ", (int)keys[i]);
            printf("\x1b[1G\n"); /* go to first column of new line */

            if (keys[0] == 3) /* ctrl-c. may cause signal instead */
                return -1;
        }
    }
#endif

    if (c == 27) { /* escape */
        if (read(fd,seq,2) == -1) return 0;
        if (seq[0] == 91){
            if (seq[1] == 68) { /* left arrow */
                return 2; /* ctrl-b */
            } else if (seq[1] == 67) { /* right arrow */
                return 6; /* ctrl-f */
            } else if (seq[1] == 65) { /* up arrow */
                return 16; /* ctrl-p */
            } else if (seq[1] == 66) { /* down arrow */
                return 14; /* ctrl-n */
            } else if (seq[1] > 48 && seq[1] < 57) {
                /* extended escape */
                if (read(fd,seq2,2) == -1) return 0;
                if (seq2[0] == 126) {
                    if (seq[1] == 49 || seq[1] == 55) { /* home (linux console and rxvt based) */
                        return 1; /* ctrl-a */
                    } else if (seq[1] == 52 || seq[1] == 56 ) { /* end (linux console and rxvt based) */
                        return 5; /* ctrl-e */
                    } else if (seq[1] == 51) { /* delete */
                        return 127; /* ascii DEL byte */
                    } else {
                        return -1;
                    }
                } else {
                    return -1;
                }
                if (seq[1] == 51 && seq2[0] == 126) { /* delete */
                    return 127; /* ascii DEL byte */
                } else {
                    return -1;
                }
            } else if (seq[1] == 72) { /* home (konsole) */
                return 1; /* ctrl-a */
            } else if (seq[1] == 70) { /* end (konsole) */
                return 5; /* ctrl-e */
            } else {
                return -1;
            }
        } else if (seq[0] == 79){
            if (seq[1] == 72) { /* home (xterm based) */
                return 1; /* ctrl-a */
            } else if (seq[1] == 70) { /* end (xterm based) */
                return 5; /* ctrl-e */
            } else {
                return -1;
            }
        } else {
            return -1;
        }
    } else if (c == 127) {
        /* some consoles use 127 for backspace rather than delete.
         * we only use it for delete */
        return 8;
    }

    return c; /* normalish character */
#endif
}

static void beep() {
    /* doesn't do anything on windows but harmless */
    fprintf(stderr, "\x7");
    fflush(stderr);
}

static void freeCompletions(linenoiseCompletions *lc) {
    size_t i;
    for (i = 0; i < lc->len; i++)
        free(lc->cvec[i]);
    if (lc->cvec != NULL)
        free(lc->cvec);
}

// break characters that may precede items to be completed
static const char breakChars[] = " =+-/\\*?\"'`&<>;|@{([])}";

/**
 * Handle command completion, using a completionCallback() routine to provide possible substitutions
 * This routine handles the mechanics of updating the user's input buffer with possible replacement of
 * text as the user selects a proposed completion string, or cancels the completion attempt.
 * @param fd     file handle to use for output to the screen
 * @param pi     PROMPTINFO struct holding information about the prompt and our screen position
 * @param buf    input buffer to be displayed
 * @param buflen size of input buffer in bytes
 * @param len    ptr to count of characters in the buffer (updated)
 * @param pos    ptr to current cursor position within the buffer (0 <= pos <= len) (updated)
 * @param cols   screen width in columns
 */
static int completeLine(int fd, PROMPTINFO & pi, char *buf, int buflen, int *len, int *pos, int cols) {
    linenoiseCompletions lc = { 0, NULL };
    char c = 0;

    // completionCallback() expects a parsable entity, so find the previous break character and extract
    // a copy to parse.  we also handle the case where tab is hit while not at end-of-line.
    int startIndex = *pos;
    while ( --startIndex >= 0 ) {
        if ( strchr(breakChars, buf[startIndex]) ) {
            break;
        }
    }
    ++startIndex;
    int itemLength = *pos - startIndex;
    char * parseItem = (char *)malloc( itemLength + 1 );
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
        size_t stop = 0, i = 0;
        size_t clen;

        while ( !stop ) {
            /* Show completion or original buffer */
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
                refreshLine( fd, pi, displayText, displayLength, startIndex + clen, cols );
                free( displayText );
            }
            else {
                refreshLine( fd, pi, buf, *len, *pos, cols );
            }

            do {
                c = linenoiseReadChar( fd );
            } while ( c == (char)-1 );

            switch ( c ) {
                case 0:
                    freeCompletions( &lc );
                    return -1;
                case 9: /* tab */
                    i = ( i + 1 ) % ( lc.len + 1 );
                    if ( i == lc.len ) beep();
                    break;
                case 27: /* escape */
                    /* Re-show original buffer */
                    if ( i < lc.len )
                        refreshLine( fd, pi, buf, *len, *pos, cols );
                    stop = 1;
                    break;
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
                    stop = 1;
                    break;
            }
        }
    }

    freeCompletions( &lc );
    return c; /* Return last read character */
}

static void linenoiseClearScreen(int fd, PROMPTINFO & pi, char *buf, int len, int pos, int cols) {

#ifdef _WIN32
    COORD coord = {0, 0};
    CONSOLE_SCREEN_BUFFER_INFO inf;
    GetConsoleScreenBufferInfo(console_out, &inf);
    SetConsoleCursorPosition(console_out, coord);
    DWORD count;
    FillConsoleOutputCharacterA(console_out, ' ', inf.dwSize.X * inf.dwSize.Y, coord, &count);
#else
    if ( write(1,"\x1b[H\x1b[2J",7) <= 0 ) return;
#endif
    if ( write(1, pi.promptText, pi.promptChars) == -1 ) return;
    pi.promptCursorRowOffset = pi.promptExtraLines;
    refreshLine(fd, pi, buf, len, pos, cols);
}

static int linenoisePrompt(int fd, char *buf, int buflen, PROMPTINFO & pi) {
    int pos = 0;
    int len = 0;
    int cols = getColumns();
    // cols is 0 in certain circumstances like inside debugger, which creates further issues
    cols = cols > 0 ? cols : 80;

    buf[0] = '\0';
    buflen--; /* Make sure there is always space for the nulterm */

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    linenoiseHistoryAdd("");
    history_index = history_len-1;

    // display the prompt
    if ( write(1, pi.promptText, pi.promptChars) == -1 ) return -1;

    // the cursor starts out at the end of the prompt
    pi.promptCursorRowOffset = pi.promptExtraLines;
    while (1) {
        char c = linenoiseReadChar(fd);

        if (c == 0) return len;
        if (c == (char)-1) {
            refreshLine(fd, pi, buf, len, pos, cols);
            continue;
        }

        /* Only autocomplete when the callback is set. It returns < 0 when
         * there was an error reading from fd. Otherwise it will return the
         * character that should be handled next. */
        if (c == 9 && completionCallback != NULL) { /* tab */
            /* ignore tabs used for indentation */
            if (pos == 0) continue;

            c = completeLine(fd, pi, buf, buflen, &len, &pos, cols);
            /* Return on errors */
            if (c < 0) return len;
            /* Read next character when 0 */
            if (c == 0) continue;
        }

        switch(c) {
        case 10:
        case 13:    /* enter */
            // we need one last refresh with the cursor at the end of the line so
            // we don't display the next prompt over the previous input line
            refreshLine(fd, pi, buf, len, len, cols);  // pass len as pos for EOL
            history_len--;
            free(history[history_len]);
            return len;
        case 3:     /* ctrl-c */
            errno = EAGAIN;
            return -1;
        case 8:     /* backspace or ctrl-h */
            if (pos > 0 && len > 0) {
                memmove(buf+pos-1,buf+pos,len-pos);
                pos--;
                len--;
                buf[len] = '\0';
                refreshLine(fd, pi, buf, len, pos, cols);
            }
            break;
        case 127:  // DEL and ctrl-d both delete the character under the cursor
        case 4:    // on an empty line, DEL does nothing while ctrl-d will exit the shell
            if( len > 0 && pos < len ) {
                memmove(buf+pos,buf+pos+1,len-pos);
                len--;
                refreshLine(fd, pi, buf, len, pos, cols);
            }
            else if( c == 4 && len == 0 ) {
                history_len--;
                free(history[history_len]);
                return -1;
            }
            break;
        case 20:    /* ctrl-t */
            if (pos > 0 && len > 1) {
                size_t leftCharPos = (pos == len) ? pos - 2 : pos - 1;
                char aux = buf[leftCharPos];
                buf[leftCharPos] = buf[leftCharPos+1];
                buf[leftCharPos+1] = aux;
                if (pos != len) pos++;
                refreshLine(fd, pi ,buf, len, pos, cols);
            }
            break;
        case 2:     /* ctrl-b */ /* left arrow */
            if (pos > 0) {
                pos--;
                refreshLine(fd, pi, buf, len, pos, cols);
            }
            break;
        case 6:     /* ctrl-f */
            /* right arrow */
            if (pos != len) {
                pos++;
                refreshLine(fd, pi ,buf, len, pos, cols);
            }
            break;
        case 16:    /* ctrl-p */
        case 14:    /* ctrl-n */
            /* up and down arrow: history */
            if (history_len > 1) {
                /* Update the current history entry before to
                 * overwrite it with tne next one. */
                free(history[history_index]);
                history[history_index] = strdup(buf);
                /* Show the new entry */
                history_index += (c == 16) ? -1 : 1;
                if (history_index < 0) {
                    history_index = 0;
                    break;
                } else if (history_index >= history_len) {
                    history_index = history_len-1;
                    break;
                }
                strncpy(buf,history[history_index],buflen);
                buf[buflen] = '\0';
                len = pos = strlen(buf);
                refreshLine(fd, pi, buf, len, pos, cols);
            }
            break;
        case 27:    /* escape sequence */
            break; /* should be handled by linenoiseReadChar */
        default:
            if (len < buflen) {
                if ((unsigned char)c < 32) { /* Unhandled control character */
                    beep();
                    break;
                }
                if (len == pos) {
                    buf[pos] = c;
                    pos++;
                    len++;
                    buf[len] = '\0';
                    if ( pi.promptIndentation + len < cols ) {
                        if ( len > pi.promptPreviousInputLen )
                            pi.promptPreviousInputLen = len;
                        /* Avoid a full update of the line in the
                         * trivial case. */
                        if ( write(1,&c,1) == -1 ) return -1;
                    } else {
                        refreshLine(fd, pi, buf, len, pos, cols);
                    }
                } else {
                    memmove(buf+pos+1,buf+pos,len-pos);
                    buf[pos] = c;
                    len++;
                    pos++;
                    buf[len] = '\0';
                    refreshLine(fd, pi, buf, len, pos, cols);
                }
            }
            break;
        case 21: /* Ctrl+u, delete the whole line. */
            buf[0] = '\0';
            pos = len = 0;
            refreshLine(fd, pi, buf, len, pos, cols);
            break;
        case 11: /* Ctrl+k, delete from current to end of line. */
            buf[pos] = '\0';
            len = pos;
            refreshLine(fd, pi, buf, len, pos, cols);
            break;
        case 1: /* Ctrl+a, go to the start of the line */
            pos = 0;
            refreshLine(fd, pi ,buf, len, pos, cols);
            break;
        case 5: /* ctrl+e, go to the end of the line */
            pos = len;
            refreshLine(fd, pi, buf, len, pos, cols);
            break;
        case 12: /* ctrl+l, clear screen */
            linenoiseClearScreen(fd, pi, buf, len, pos, cols);
            break;
        }
    }
    return len;
}

static int linenoiseRaw(char *buf, int buflen, PROMPTINFO & pi) {
    int fd = STDIN_FILENO;
    int count;

    if (buflen == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!isatty(STDIN_FILENO)) {
        if (fgets(buf, buflen, stdin) == NULL) return -1;
        count = strlen(buf);
        if (count && buf[count-1] == '\n') {
            count--;
            buf[count] = '\0';
        }
    } else {
        if (enableRawMode(fd) == -1) return -1;
        count = linenoisePrompt(fd, buf, buflen, pi);
        disableRawMode(fd);
        printf("\n");
    }
    return count;
}

// the returned string is allocated with strdup() and must be freed by calling free()
char *linenoise(const char *prompt) {
    char buf[LINENOISE_MAX_LINE];
    int count;

    PROMPTINFO pi;

    // promptCopy owns the memory, pi.promptText is just a ptr to it
    boost::scoped_array<char> promptCopy(new char[strlen(prompt)+1]);
    strcpy(&promptCopy[0], prompt);
    pi.promptText = &promptCopy[0];

    // strip evil characters from the prompt -- we do allow newline
    unsigned char * pIn = reinterpret_cast<unsigned char *>(pi.promptText);
    unsigned char * pOut = pIn;
    while ( *pIn ) {
        unsigned char c = *pIn;  // we need unsigned so chars 0x80 and above are allowed
        if ( '\n' == c || c >= ' ' ) {
            *pOut = c;
            pOut++;
        }
        pIn++;
    }
    *pOut = 0;
    pi.promptChars = pOut - reinterpret_cast<unsigned char *>(pi.promptText);
    pi.promptExtraLines = 0;
    pi.promptLastLinePosition = 0;
    pi.promptPreviousInputLen = 0;
    int x = 0;
    int cols = getColumns();
    // cols is 0 in certain circumstances like inside debugger, which creates further issues
    cols = cols > 0 ? cols : 80;
    for( int i = 0; i < pi.promptChars; ++i ) {
        char c = pi.promptText[i];
        if ( '\n' == c ) {
            x = 0;
            pi.promptExtraLines++;
            pi.promptLastLinePosition = i + 1;
        }
        else {
            x++;
            if ( x >= cols ) {
                x = 0;
                pi.promptExtraLines++;
                pi.promptLastLinePosition = i + 1;
            }
        }
    }
    pi.promptIndentation = pi.promptChars - pi.promptLastLinePosition;

    if (isUnsupportedTerm()) {
        size_t len;

        printf("%s", pi.promptText);
        fflush(stdout);
        if (fgets(buf,LINENOISE_MAX_LINE,stdin) == NULL) return NULL;
        len = strlen(buf);
        while(len && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
            len--;
            buf[len] = '\0';
        }
        return strdup(buf);
    } else {
        count = linenoiseRaw(buf, LINENOISE_MAX_LINE, pi);
        if (count == -1) return NULL;
        return strdup(buf);
    }
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn) {
    completionCallback = fn;
}

void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str) {
    size_t len = strlen(str);
    char *copy = (char*)malloc(len+1);
    memcpy(copy,str,len+1);
    lc->cvec = (char**)realloc(lc->cvec,sizeof(char*)*(lc->len+1));
    lc->cvec[lc->len++] = copy;
}

/* Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(const char *line) {
    char *linecopy;

    if (history_max_len == 0) return 0;
    if (history == NULL) {
        history = (char**)malloc(sizeof(char*)*history_max_len);
        if (history == NULL) return 0;
        memset(history,0,(sizeof(char*)*history_max_len));
    }
    linecopy = strdup(line);
    if (!linecopy) return 0;
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history,history+1,sizeof(char*)*(history_max_len-1));
        history_len--;
    }

    // convert newlines in multi-line code to spaces before storing
    char *p = linecopy;
    while( *p ) {
        if( *p == '\n' ) *p = ' ';
        p++;
    }
    history[history_len] = linecopy;
    history_len++;
    return 1;
}

int linenoiseHistorySetMaxLen(int len) {
    char **newHistory;

    if (len < 1) return 0;
    if (history) {
        int tocopy = history_len;

        newHistory = (char**)malloc(sizeof(char*)*len);
        if (newHistory == NULL) return 0;
        if (len < tocopy) tocopy = len;
        memcpy(newHistory,history+(history_max_len-tocopy), sizeof(char*)*tocopy);
        free(history);
        history = newHistory;
    }
    history_max_len = len;
    if (history_len > history_max_len)
        history_len = history_max_len;
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(const char *filename) {
    FILE *fp = fopen(filename,"w");
    int j;
    
    if (fp == NULL) return -1;
    for (j = 0; j < history_len; j++){
        if (history[j][0] != '\0')
            fprintf(fp,"%s\n",history[j]);
    }
    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(const char *filename) {
    FILE *fp = fopen(filename,"r");
    char buf[LINENOISE_MAX_LINE];
    
    if (fp == NULL) return -1;

    while (fgets(buf,LINENOISE_MAX_LINE,fp) != NULL) {
        char *p;
        
        p = strchr(buf,'\r');
        if (!p) p = strchr(buf,'\n');
        if (p) *p = '\0';
        if (p != buf)
            linenoiseHistoryAdd(buf);
    }
    fclose(fp);
    return 0;
}
