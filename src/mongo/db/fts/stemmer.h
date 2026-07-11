// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/db/fts/fts_language.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <libstemmer.h>


namespace mongo::fts {

/**
 * maintains case
 * but works
 * running/Running -> run/Run
 */
class Stemmer {
public:
    explicit Stemmer(const FTSLanguage* language);
    ~Stemmer();
    Stemmer(Stemmer&&) = default;
    Stemmer& operator=(Stemmer&&) = default;

    /**
     * Stems an input word.
     *
     * The returned std::string_view is valid until the next call to any method on this object.
     * Since the input may be returned unmodified, the output's lifetime may also expire when the
     * input's does.
     */
    std::string_view stem(std::string_view word) const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace mongo::fts
