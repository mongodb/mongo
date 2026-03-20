/**
 *
 * Copyright 2023 Ribose Inc.
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
 **/

#include "sexpp/sexp.h"

namespace sexp {

sexp_depth_manager::sexp_depth_manager(size_t m_depth)
{
    reset_depth(m_depth);
}
void sexp_depth_manager::reset_depth(size_t m_depth)
{
    depth = 0;
    max_depth = m_depth;
}
void sexp_depth_manager::increase_depth(int count)
{
    if (max_depth != 0 && ++depth > max_depth)
        sexp_error(sexp_exception_t::error,
                   "Maximum allowed SEXP list depth (%zu) is exceeded",
                   max_depth,
                   0,
                   count);
}
void sexp_depth_manager::decrease_depth(void)
{
    depth--;
}
} // namespace sexp
