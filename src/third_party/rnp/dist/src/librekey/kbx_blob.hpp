/*
 * Copyright (c) 2021, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE RIBOSE, INC. AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RNP_KBX_BLOB_HPP
#define RNP_KBX_BLOB_HPP

#include <vector>
#include "repgp/repgp_def.h"
#include "fingerprint.hpp"

typedef enum : uint8_t {
    KBX_EMPTY_BLOB = 0,
    KBX_HEADER_BLOB = 1,
    KBX_PGP_BLOB = 2,
    KBX_X509_BLOB = 3
} kbx_blob_type_t;

class kbx_blob_t {
  protected:
    kbx_blob_type_t      type_;
    std::vector<uint8_t> image_;

    uint8_t  ru8(size_t idx);
    uint16_t ru16(size_t idx);
    uint32_t ru32(size_t idx);

  public:
    virtual ~kbx_blob_t() = default;
    kbx_blob_t(std::vector<uint8_t> &data);
    virtual bool
    parse()
    {
        return true;
    };

    kbx_blob_type_t
    type()
    {
        return type_;
    }

    std::vector<uint8_t> &
    image()
    {
        return image_;
    }

    uint32_t
    length() const noexcept
    {
        return image_.size();
    }
};

class kbx_header_blob_t : public kbx_blob_t {
  protected:
    uint8_t  version_{};
    uint16_t flags_{};
    uint32_t file_created_at_{};
    uint32_t last_maintenance_run_{};

  public:
    kbx_header_blob_t(std::vector<uint8_t> &data) : kbx_blob_t(data){};
    bool parse();

    uint32_t
    file_created_at()
    {
        return file_created_at_;
    }
};

typedef struct {
    uint8_t  fp[PGP_MAX_FINGERPRINT_SIZE];
    uint32_t keyid_offset;
    uint16_t flags;
} kbx_pgp_key_t;

typedef struct {
    uint32_t offset;
    uint32_t length;
    uint16_t flags;
    uint8_t  validity;
} kbx_pgp_uid_t;

typedef struct {
    uint32_t expired;
} kbx_pgp_sig_t;

class kbx_pgp_blob_t : public kbx_blob_t {
  protected:
    uint8_t  version_{};
    uint16_t flags_{};
    uint32_t keyblock_offset_{};
    uint32_t keyblock_length_{};

    std::vector<uint8_t>       sn_{};
    std::vector<kbx_pgp_key_t> keys_{};
    std::vector<kbx_pgp_uid_t> uids_{};
    std::vector<kbx_pgp_sig_t> sigs_{};

    uint8_t ownertrust_{};
    uint8_t all_validity_{};

    uint32_t recheck_after_{};
    uint32_t latest_timestamp_{};
    uint32_t blob_created_at_{};

  public:
    kbx_pgp_blob_t(std::vector<uint8_t> &data) : kbx_blob_t(data){};

    uint32_t
    keyblock_offset()
    {
        return keyblock_offset_;
    }

    uint32_t
    keyblock_length()
    {
        return keyblock_length_;
    }

    size_t
    nkeys()
    {
        return keys_.size();
    }
    size_t
    nuids()
    {
        return uids_.size();
    }
    size_t
    nsigs()
    {
        return sigs_.size();
    }

    bool parse();
};

#endif
