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

#include "sexpp/sexp-error.h"

namespace sexp {

sexp_exception_t::severity sexp_exception_t::verbosity = sexp_exception_t::error;
bool                       sexp_exception_t::interactive = false;

std::string sexp_exception_t::format(std::string prf,
                                     std::string message,
                                     severity    level,
                                     int         position)
{
    std::string r = prf + (level == error ? " ERROR: " : " WARNING: ") + message;
    if (position >= 0)
        r += " at position " + std::to_string(position);
    return r;
};

void sexp_error(sexp_exception_t::severity level, const char *msg, int pos)
{
    sexp_exception_t::severity l = (sexp_exception_t::severity) level;
    if (sexp_exception_t::shall_throw(l))
        throw sexp_exception_t(msg, l, pos);
    if (sexp_exception_t::is_interactive()) {
        std::cout.flush() << std::endl
                          << "*** " << sexp_exception_t::format("SEXP", msg, l, pos) << " ***"
                          << std::endl;
    }
}

void sexp_error(sexp_exception_t::severity level, const char *msg, size_t c1, int pos)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp) / sizeof(tmp[0]), msg, c1);
    sexp_error(level, tmp, pos);
}

void sexp_error(
  sexp_exception_t::severity level, const char *msg, size_t c1, size_t c2, int pos)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp) / sizeof(tmp[0]), msg, c1, c2);
    sexp_error(level, tmp, pos);
}

} // namespace sexp
