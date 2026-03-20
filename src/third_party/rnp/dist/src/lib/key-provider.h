/*
 * Copyright (c) 2017, [Ribose Inc](https://www.ribose.com).
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
#ifndef RNP_KEY_PROVIDER_H
#define RNP_KEY_PROVIDER_H

#include "types.h"
#include "fingerprint.hpp"

namespace rnp {
class Key;
}

typedef struct pgp_key_request_ctx_t pgp_key_request_ctx_t;

typedef rnp::Key *pgp_key_callback_t(const pgp_key_request_ctx_t *ctx, void *userdata);

namespace rnp {

class KeySearch {
  public:
    enum class Type { Unknown, KeyID, Fingerprint, Grip, UserID };
    static Type find_type(const std::string &name);

    virtual Type
    type() const
    {
        return type_;
    }
    virtual bool              matches(const Key &key) const = 0;
    virtual const std::string name() const = 0;
    virtual std::string       value() const = 0;
    virtual ~KeySearch() = default;

    static std::unique_ptr<KeySearch> create(const pgp::KeyID &keyid);
    static std::unique_ptr<KeySearch> create(const pgp::Fingerprint &fp);
    static std::unique_ptr<KeySearch> create(const pgp::KeyGrip &grip);
    static std::unique_ptr<KeySearch> create(const std::string &uid);
    static std::unique_ptr<KeySearch> create(const std::string &name,
                                             const std::string &value);

  protected:
    Type type_;
};

class KeyIDSearch : public KeySearch {
    pgp::KeyID keyid_;

  public:
    bool              matches(const Key &key) const;
    const std::string name() const;
    std::string       value() const;
    bool              hidden() const;

    KeyIDSearch(const pgp::KeyID &keyid);
};

class KeyFingerprintSearch : public KeySearch {
    pgp::Fingerprint fp_;

  public:
    bool              matches(const Key &key) const;
    const std::string name() const;
    std::string       value() const;

    KeyFingerprintSearch(const pgp::Fingerprint &fp);
    const pgp::Fingerprint &get_fp() const;
};

class KeyGripSearch : public KeySearch {
    pgp::KeyGrip grip_;

  public:
    bool              matches(const Key &key) const;
    const std::string name() const;
    std::string       value() const;

    KeyGripSearch(const pgp::KeyGrip &grip);
};

class KeyUIDSearch : public KeySearch {
    std::string uid_;

  public:
    bool              matches(const Key &key) const;
    const std::string name() const;
    std::string       value() const;

    KeyUIDSearch(const std::string &uid);
};

class KeyProvider {
  public:
    pgp_key_callback_t *callback;
    void *              userdata;

    KeyProvider(pgp_key_callback_t *cb = nullptr, void *ud = nullptr)
        : callback(cb), userdata(ud){};

    /** @brief request public or secret pgp key, according to parameters
     *  @param search search object
     *  @param op for which operation key is requested
     *  @param secret whether secret key is requested
     *  @return a key pointer on success, or nullptr if key was not found otherwise
     **/
    Key *request_key(const KeySearch &search,
                     pgp_op_t         op = PGP_OP_UNKNOWN,
                     bool             secret = false) const;
};
} // namespace rnp

typedef struct pgp_key_request_ctx_t {
    pgp_op_t              op;
    bool                  secret;
    const rnp::KeySearch &search;

    pgp_key_request_ctx_t(pgp_op_t anop, bool sec, const rnp::KeySearch &srch)
        : op(anop), secret(sec), search(srch)
    {
    }
} pgp_key_request_ctx_t;

/** key provider callback that searches a list of rnp::Key pointers
 *
 *  @param ctx
 *  @param userdata must be a list of key rnp::Key**
 */
rnp::Key *rnp_key_provider_key_ptr_list(const pgp_key_request_ctx_t *ctx, void *userdata);

/** key provider callback that searches a given store
 *
 *  @param ctx
 *  @param userdata must be a pointer to rnp::KeyStore
 */
rnp::Key *rnp_key_provider_store(const pgp_key_request_ctx_t *ctx, void *userdata);

/** key provider that calls other key providers
 *
 *  @param ctx
 *  @param userdata must be an array rnp::KeyProvider pointers,
 *         ending with a nullptr.
 */
rnp::Key *rnp_key_provider_chained(const pgp_key_request_ctx_t *ctx, void *userdata);

#endif
