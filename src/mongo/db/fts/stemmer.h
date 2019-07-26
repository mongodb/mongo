/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/fts/fts_language.h"
#include "third_party/libstemmer_c/include/libstemmer.h"

namespace mongo {

namespace fts {

/**
 * maintains case
 * but works
 * running/Running -> run/Run
 */
class Stemmer {
    Stemmer(const Stemmer&) = delete;
    Stemmer& operator=(const Stemmer&) = delete;

public:
    Stemmer(const FTSLanguage* language);
    ~Stemmer();

    /**
     * Stems an input word.
     *
     * The returned StringData is valid until the next call to any method on this object. Since the
     * input may be returned unmodified, the output's lifetime may also expire when the input's
     * does.
     */
    StringData stem(StringData word) const;

private:
    struct sb_stemmer* _stemmer;
};
}  // namespace fts
}  // namespace mongo
