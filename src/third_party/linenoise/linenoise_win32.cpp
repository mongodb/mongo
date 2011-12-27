/* linenoise_win32.c -- Linenoise win32 port.
 *
 * Modifications copyright 2010, Jon Griffiths <jon_p_griffiths at yahoo dot com>.
 * All rights reserved.
 * Based on linenoise, copyright 2010, Salvatore Sanfilippo <antirez at gmail dot com>.
 * The original linenoise can be found at: http://github.com/antirez/linenoise
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
 * Todo list:
 * Actually switch to/from raw mode so emacs key combos work.
 * Set a console handler to clean up onn exit.
 */
#include <conio.h>
#include <windows.h>
#include <stdio.h>

/* If ALT_KEYS is defined, emacs key combos using ALT instead of CTRL are
 * available. At this time, you don't get key repeats when enabled though. */
/* #define ALT_KEYS */

static HANDLE console_in, console_out;

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096

static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
char** history = NULL;

int linenoiseHistoryAdd(const char* line);

static int enableRawMode()
{
    if (!console_in)
        {
            console_in = GetStdHandle(STD_INPUT_HANDLE);
            console_out = GetStdHandle(STD_OUTPUT_HANDLE);
        }
    return 0;
}

static void disableRawMode()
{
    /* Nothing to do yet */
}

static void output(const char* str,
                   size_t      len,
                   int         x,
                   int         y)
{
    COORD pos = { (SHORT)x, (SHORT)y };
    DWORD count = 0;
    WriteConsoleOutputCharacterA(console_out, str, len, pos, &count);
}

static void refreshLine(const char* prompt,
                        char*       buf,
                        size_t      len,
                        size_t      pos,
                        size_t      cols)
{
    size_t plen = strlen(prompt);

    while ((plen + pos) >= cols)
        {
            buf++;
            len--;
            pos--;
        }
    while (plen + len > cols)
        {
            len--;
        }

    CONSOLE_SCREEN_BUFFER_INFO inf = { 0 };
    GetConsoleScreenBufferInfo(console_out, &inf);
    size_t prompt_len = strlen(prompt);
    output(prompt, prompt_len, 0, inf.dwCursorPosition.Y);
    output(buf, len, prompt_len, inf.dwCursorPosition.Y);
    if (prompt_len + len < (size_t)inf.dwSize.X)
        {
            /* Blank to EOL */
            char* tmp = (char*)malloc(inf.dwSize.X - (prompt_len + len));
            memset(tmp, ' ', inf.dwSize.X - (prompt_len + len));
            output(tmp, inf.dwSize.X - (prompt_len + len), len + prompt_len, inf.dwCursorPosition.Y);
            free(tmp);
        }
    inf.dwCursorPosition.X = (SHORT)(pos + prompt_len);
    SetConsoleCursorPosition(console_out, inf.dwCursorPosition);
}

static int linenoisePrompt(char*       buf,
                           size_t      buflen,
                           const char* prompt)
{
    size_t plen = strlen(prompt);
    size_t pos = 0;
    size_t len = 0;
    int history_index = 0;
#ifdef ALT_KEYS
    unsigned char last_down = 0;
#endif
    buf[0] = '\0';
    buflen--; /* Make sure there is always space for the nulterm */

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    linenoiseHistoryAdd("");

    CONSOLE_SCREEN_BUFFER_INFO inf = { 0 };
    GetConsoleScreenBufferInfo(console_out, &inf);
    size_t cols = inf.dwSize.X;
    output(prompt, plen, 0, inf.dwCursorPosition.Y);
    inf.dwCursorPosition.X = (SHORT)plen;
    SetConsoleCursorPosition(console_out, inf.dwCursorPosition);

    for ( ; ; )
        {
            INPUT_RECORD rec;
            DWORD count;
            ReadConsoleInputA(console_in, &rec, 1, &count);
            if (rec.EventType != KEY_EVENT)
                continue;
#ifdef ALT_KEYS
            if (rec.Event.KeyEvent.bKeyDown)
                {
                    last_down = rec.Event.KeyEvent.uChar.AsciiChar;
                    continue;
                }
#else
            if (!rec.Event.KeyEvent.bKeyDown)
                {
                    continue;
                }
#endif
            switch (rec.Event.KeyEvent.wVirtualKeyCode)
                {
                case VK_RETURN:    /* enter */
                    history_len--;
                    free(history[history_len]);
                    return (int)len;
                case VK_BACK:   /* backspace */
#ifdef ALT_KEYS
                backspace:
#endif
                    if (pos > 0 && len > 0)
                        {
                            memmove(buf + pos - 1, buf + pos, len - pos);
                            pos--;
                            len--;
                            buf[len] = '\0';
                            refreshLine(prompt, buf, len, pos, cols);
                        }
                    break;
                case VK_LEFT:
#ifdef ALT_KEYS
                left_arrow:
#endif
                    /* left arrow */
                    if (pos > 0)
                        {
                            pos--;
                            refreshLine(prompt, buf, len, pos, cols);
                        }
                    break;
                case VK_RIGHT:
#ifdef ALT_KEYS
                right_arrow:
#endif
                    /* right arrow */
                    if (pos != len)
                        {
                            pos++;
                            refreshLine(prompt, buf, len, pos, cols);
                        }
                    break;
                case VK_UP:
                case VK_DOWN:
#ifdef ALT_KEYS
                up_down_arrow:
#endif
                    /* up and down arrow: history */
                    if (history_len > 1)
                        {
                            /* Update the current history entry before to
                             * overwrite it with tne next one. */
                            free(history[history_len - 1 - history_index]);
                            history[history_len - 1 - history_index] = _strdup(buf);
                            /* Show the new entry */
                            history_index += (rec.Event.KeyEvent.wVirtualKeyCode == VK_UP) ? 1 : -1;
                            if (history_index < 0)
                                {
                                    history_index = 0;
                                    break;
                                }
                            else if (history_index >= history_len)
                                {
                                    history_index = history_len - 1;
                                    break;
                                }
                            strncpy(buf, history[history_len - 1 - history_index], buflen);
                            buf[buflen] = '\0';
                            len = pos = strlen(buf);
                            refreshLine(prompt, buf, len, pos, cols);
                        }
                    break;
                case VK_DELETE:
                    /* delete */
                    if (len > 0 && pos < len)
                        {
                            memmove(buf + pos, buf + pos + 1, len - pos - 1);
                            len--;
                            buf[len] = '\0';
                            refreshLine(prompt, buf, len, pos, cols);
                        }
                    break;
                case VK_HOME: /* Ctrl+a, go to the start of the line */
#ifdef ALT_KEYS
                home:
#endif
                    pos = 0;
                    refreshLine(prompt, buf, len, pos, cols);
                    break;
                case VK_END: /* ctrl+e, go to the end of the line */
#ifdef ALT_KEYS
                end:
#endif
                    pos = len;
                    refreshLine(prompt, buf, len, pos, cols);
                    break;
                default:
#ifdef ALT_KEYS
                    /* Use alt instead of CTRL since windows eats CTRL+char combos */
                    if (rec.Event.KeyEvent.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED))
                        {
                            switch (last_down)
                                {
                                case 'a': /* ctrl-t */
                                    goto home;
                                case 'e': /* ctrl-t */
                                    goto end;
                                case 't': /* ctrl-t */
                                    if (pos > 0 && pos < len)
                                        {
                                            int aux = buf[pos - 1];
                                            buf[pos - 1] = buf[pos];
                                            buf[pos] = aux;
                                            if (pos != len - 1)
                                                pos++;
                                            refreshLine(prompt, buf, len, pos, cols);
                                        }
                                    break;
                                case 'h': /* ctrl-h */
                                    goto backspace;
                                case 'b': /* ctrl-b */
                                    goto left_arrow;
                                case 'f': /* ctrl-f */
                                    goto right_arrow;
                                case 'p': /* ctrl-p */
                                    rec.Event.KeyEvent.wVirtualKeyCode = VK_UP;
                                    goto up_down_arrow;
                                case 'n': /* ctrl-n */
                                    rec.Event.KeyEvent.wVirtualKeyCode = VK_DOWN;
                                    goto up_down_arrow;
                                case 'u': /* Ctrl+u, delete the whole line. */
                                    buf[0] = '\0';
                                    pos = len = 0;
                                    refreshLine(prompt, buf, len, pos, cols);
                                    break;
                                case 'k': /* Ctrl+k, delete from current to end of line. */
                                    buf[pos] = '\0';
                                    len = pos;
                                    refreshLine(prompt, buf, len, pos, cols);
                                    break;
                                }
                            continue;
                        }
#endif /* ALT_KEYS */
                    if (rec.Event.KeyEvent.uChar.AsciiChar < ' ' ||
                        rec.Event.KeyEvent.uChar.AsciiChar > '~')
                        continue;

                    if (len < buflen)
                        {
                            if (len != pos)
                                memmove(buf + pos + 1, buf + pos, len - pos);
                            buf[pos] = rec.Event.KeyEvent.uChar.AsciiChar;
                            len++;
                            pos++;
                            buf[len] = '\0';
                            refreshLine(prompt, buf, len, pos, cols);
                        }
                    break;
                }
        }
}

static int linenoiseRaw(char*       buf,
                        size_t      buflen,
                        const char* prompt)
{
    int count = -1;

    if (buflen != 0)
        {
            if (enableRawMode() == -1)
                return -1;
            count = linenoisePrompt(buf, buflen, prompt);
            disableRawMode();
            printf("\n");
        }
    return count;
}

char* linenoise(const char* prompt)
{
    char buf[LINENOISE_MAX_LINE];
    int count = linenoiseRaw(buf, LINENOISE_MAX_LINE, prompt);
    if (count == -1)
        return NULL;
    return _strdup(buf);
}

/* Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(const char* line)
{
    char* linecopy;

    if (history_max_len == 0)
        return 0;
    if (history == NULL)
        {
            history = (char**)malloc(sizeof(char*) * history_max_len);
            if (history == NULL)
                return 0;
            memset(history, 0, (sizeof(char*) * history_max_len));
        }
    linecopy = _strdup(line);
    if (!linecopy)
        return 0;
    if (history_len == history_max_len)
        {
            free(history[0]);
            memmove(history, history + 1, sizeof(char*) * (history_max_len - 1));
            history_len--;
        }
    history[history_len] = linecopy;
    history_len++;
    return 1;
}

int linenoiseHistorySetMaxLen(int len)
{
    char** new_history;

    if (len < 1)
        return 0;
    if (history)
        {
            int tocopy = history_len;

            new_history = (char**)malloc(sizeof(char*) * len);
            if (new_history == NULL)
                return 0;
            if (len < tocopy)
                tocopy = len;
            memcpy(new_history, history + (history_max_len - tocopy), sizeof(char*) * tocopy);
            free(history);
            history = new_history;
        }
    history_max_len = len;
    if (history_len > history_max_len)
        history_len = history_max_len;
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(const char* filename)
{
    FILE* fp = fopen(filename, "w");
    int j;

    if (fp == NULL)
        return -1;
    for (j = 0; j < history_len; j++)
        fprintf(fp, "%s\n", history[j]);
    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(const char* filename)
{
    FILE* fp = fopen(filename, "r");
    char buf[LINENOISE_MAX_LINE];

    if (fp == NULL)
        return -1;

    while (fgets(buf, LINENOISE_MAX_LINE, fp) != NULL)
        {
            char* p;

            p = strchr(buf, '\r');
            if (!p)
                p = strchr(buf, '\n');
            if (p)
                *p = '\0';
            linenoiseHistoryAdd(buf);
        }
    fclose(fp);
    return 0;
}
