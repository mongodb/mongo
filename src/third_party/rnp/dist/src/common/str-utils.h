/*
 * Copyright (c) 2019-2020 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RNP_STR_UTILS_H_
#define RNP_STR_UTILS_H_

#include <string>

namespace rnp {
char *strip_eol(char *s);
/**
 * @brief Strip EOL characters from the string's end.
 *
 * @param s string to check
 * @return true if EOL was found and stripped, or false otherwise.
 */
bool strip_eol(std::string &s);
bool is_blank_line(const char *line, size_t len);
bool str_case_eq(const char *s1, const char *s2);
bool str_case_eq(const std::string &s1, const std::string &s2);

bool        is_hex(const std::string &s);
std::string strip_hex(const std::string &s);

/**
 * @brief Convert string to lowercase and return it.
 */
char *lowercase(char *s);
bool  str_to_int(const std::string &s, int &val);
bool  is_slash(char c);
} // namespace rnp
#ifdef _WIN32
std::wstring wstr_from_utf8(const char *s);
std::wstring wstr_from_utf8(const char *first, const char *last);
std::wstring wstr_from_utf8(const std::string &s);
std::string  wstr_to_utf8(const wchar_t *ws);
std::string  wstr_to_utf8(const std::wstring &ws);
#endif // _WIN32
#endif
