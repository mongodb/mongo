/**
 *
 * Copyright 2021-2025 Ribose Inc. (https://www.ribose.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#pragma once

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <cstdint>

#include "sexp-public.h"

namespace sexp {

class SEXP_PUBLIC_SYMBOL sexp_exception_t : public std::exception {
  public:
    enum severity { error = 0, warning = 1 };

  protected:
    static severity verbosity;
    static bool     interactive;

    int         position; // May be EOF aka -1
    severity    level;
    std::string message;

  public:
    sexp_exception_t(std::string error_message,
                     severity    error_level,
                     int         error_position,
                     const char *prefix = "SEXP")
        : position{error_position}, level{error_level},
          message{format(prefix, std::move(error_message), error_level, error_position)} {};

    static std::string format(std::string prf,
                              std::string message,
                              severity    level,
                              int         position);

    static bool shall_throw(severity level) { return level == error || verbosity != error; };
    virtual const char *what(void) const throw() { return message.c_str(); };
    severity            get_level(void) const { return level; };
    uint32_t            get_position(void) const { return position; };
    static severity     get_verbosity(void) { return verbosity; };
    static bool         is_interactive(void) { return interactive; };
    static void         set_verbosity(severity new_verbosity) { verbosity = new_verbosity; };
    static void set_interactive(bool new_interactive) { interactive = new_interactive; };
};

void sexp_error(sexp_exception_t::severity level, const char *msg, int pos);

// The next two functions that are used to format error messages are used with c1, c2 values
// that are either real sizes [%zu format] or known to be characters [%c, %x, %o formats].

void sexp_error(sexp_exception_t::severity level,
                                   const char *msg,
                                   size_t c1,
                                   int pos);

// Keep the only function public to keep ABI unchanged
void SEXP_PUBLIC_SYMBOL sexp_error(sexp_exception_t::severity level,
                                   const char *msg,
                                   size_t c1,
                                   size_t c2,
                                   int pos);

} // namespace sexp
