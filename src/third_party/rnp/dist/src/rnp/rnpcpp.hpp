/*
 * Copyright (c) 2023, [Ribose Inc](https://www.ribose.com).
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef RNPCPP_HPP_
#define RNPCPP_HPP_

#include <cstring>
#include <string>
#include <memory>
#include <stdexcept>
#include "rnp/rnp.h"
#include "rnp/rnp_err.h"

namespace rnpffi {

class ffi_exception : public std::exception {
    rnp_result_t code_;
    const char * func_;

  public:
    ffi_exception(rnp_result_t code = RNP_ERROR_GENERIC, const char *func = "")
        : code_(code), func_(func){};
    virtual const char *
    what() const throw()
    {
        return "ffi_exception";
    };

    rnp_result_t
    code() const
    {
        return code_;
    };

    const char *
    func() const
    {
        return func_;
    }
};

#define CALL_FFI(func_call)                           \
    do {                                              \
        auto __ret__ = func_call;                     \
        if (__ret__) {                                \
            throw ffi_exception(__ret__, #func_call); \
        }                                             \
    } while (0)

/* Self-destroying char* wrapper to use in FFI API calls */
class String {
    char *c_str_;
    bool  secure_;

  public:
    String(bool secure = false) : c_str_(nullptr), secure_(secure)
    {
    }

    /* Do not allow to copy/move it over */
    String(const String &) = delete;
    String(String &&) = delete;
    String &operator=(const String &) = delete;
    String &operator=(String &&) = delete;

    ~String()
    {
        if (secure_ && c_str_) {
            rnp_buffer_clear(c_str_, strlen(c_str_) + 1);
        }
        rnp_buffer_destroy(c_str_);
    }

    char **
    set()
    {
        if (!c_str_) {
            return &c_str_;
        }
        if (secure_) {
            rnp_buffer_clear(c_str_, strlen(c_str_));
        }
        rnp_buffer_destroy(c_str_);
        c_str_ = NULL;
        return &c_str_;
    }

    const char *
    c_str() const
    {
        return c_str_;
    }

    char *
    c_str()
    {
        return c_str_;
    }

    std::string
    str() const
    {
        return std::string(c_str_ ? c_str_ : "");
    }
};

class Key {
    rnp_key_handle_t handle_;
    bool             own_;

  public:
    Key() = delete;
    Key(const Key &) = delete;
    Key(Key &&) = delete;
    Key &operator=(const Key &) = delete;
    Key &operator=(Key &&) = delete;

    Key(rnp_key_handle_t handle, bool own = true)
    {
        if (!handle) {
            throw std::invalid_argument("handle");
        }
        handle_ = handle;
        own_ = own;
    }

    ~Key()
    {
        if (own_) {
            rnp_key_handle_destroy(handle_);
        }
    }

    rnp_key_handle_t
    handle()
    {
        return handle_;
    }

    rnp_key_handle_t
    release()
    {
        if (!own_) {
            throw std::invalid_argument("own");
        }
        own_ = false;
        return handle_;
    }

    bool
    secret()
    {
        bool res = false;
        CALL_FFI(rnp_key_have_secret(handle_, &res));
        return res;
    }

    bool
    is_sub()
    {
        bool subkey = false;
        CALL_FFI(rnp_key_is_sub(handle_, &subkey));
        return subkey;
    }

    bool
    is_primary()
    {
        bool primary = false;
        CALL_FFI(rnp_key_is_primary(handle_, &primary));
        return primary;
    }

    std::string
    alg()
    {
        String res;
        CALL_FFI(rnp_key_get_alg(handle_, res.set()));
        return res.str();
    }

    std::string
    curve()
    {
        String res;
        CALL_FFI(rnp_key_get_curve(handle_, res.set()));
        return res.str();
    }

    std::string
    keyid()
    {
        String res;
        CALL_FFI(rnp_key_get_keyid(handle_, res.set()));
        return res.str();
    }

    std::string
    fprint()
    {
        String res;
        CALL_FFI(rnp_key_get_fprint(handle_, res.set()));
        return res.str();
    }

    std::string
    grip()
    {
        String res;
        CALL_FFI(rnp_key_get_grip(handle_, res.set()));
        return res.str();
    }

    std::string
    primary_grip()
    {
        String res;
        CALL_FFI(rnp_key_get_primary_grip(handle_, res.set()));
        return res.str();
    }

    size_t
    uid_count()
    {
        size_t res = 0;
        CALL_FFI(rnp_key_get_uid_count(handle_, &res));
        return res;
    }

    std::string
    uid_at(size_t idx)
    {
        String res;
        CALL_FFI(rnp_key_get_uid_at(handle_, idx, res.set()));
        return res.str();
    }

    bool
    is_protected()
    {
        bool prot = false;
        CALL_FFI(rnp_key_is_protected(handle_, &prot));
        return prot;
    }

    std::string
    protection_hash()
    {
        String res;
        CALL_FFI(rnp_key_get_protection_hash(handle_, res.set()));
        return res.str();
    }

    std::string
    protection_cipher()
    {
        String res;
        CALL_FFI(rnp_key_get_protection_cipher(handle_, res.set()));
        return res.str();
    }

    size_t
    protection_iterations()
    {
        size_t iterations = 0;
        CALL_FFI(rnp_key_get_protection_iterations(handle_, &iterations));
        return iterations;
    }

    bool
    is_25519_bits_tweaked()
    {
        bool tweaked = false;
        CALL_FFI(rnp_key_25519_bits_tweaked(handle_, &tweaked));
        return tweaked;
    }

    bool
    do_25519_bits_tweak()
    {
        return !rnp_key_25519_bits_tweak(handle_);
    }

    bool
    unlock(const std::string &password)
    {
        return !rnp_key_unlock(handle_, password.c_str());
    }

    bool
    unlock()
    {
        return !rnp_key_unlock(handle_, NULL);
    }

    void
    lock()
    {
        rnp_key_lock(handle_);
    }

    bool
    unprotect(const std::string &password)
    {
        return !rnp_key_unprotect(handle_, password.c_str());
    }

    bool
    protect(const std::string &password,
            const std::string &cipher,
            const std::string &hash,
            size_t             iterations)
    {
        return !rnp_key_protect(
          handle_, password.c_str(), cipher.c_str(), NULL, hash.c_str(), iterations);
    }
};

class IdentifierIterator {
    rnp_identifier_iterator_t handle_;

  public:
    IdentifierIterator() = delete;
    IdentifierIterator(const IdentifierIterator &) = delete;
    IdentifierIterator(IdentifierIterator &&) = delete;
    IdentifierIterator &operator=(const IdentifierIterator &) = delete;
    IdentifierIterator &operator=(IdentifierIterator &&) = delete;

    IdentifierIterator(rnp_identifier_iterator_t handle)
    {
        if (!handle) {
            throw std::invalid_argument("handle");
        }
        handle_ = handle;
    }

    ~IdentifierIterator()
    {
        rnp_identifier_iterator_destroy(handle_);
    }

    bool
    next(std::string &val)
    {
        val = "";
        const char *n_val = NULL;
        if (rnp_identifier_iterator_next(handle_, &n_val) || !n_val) {
            return false;
        }
        val = n_val;
        return true;
    }
};

class FFI {
    rnp_ffi_t handle_;
    bool      own_;

  public:
    FFI() = delete;
    FFI(const FFI &) = delete;
    FFI(FFI &&) = delete;
    FFI &operator=(const FFI &) = delete;
    FFI &operator=(FFI &&) = delete;

    FFI(rnp_ffi_t handle, bool own = true)
    {
        if (!handle) {
            throw std::invalid_argument("handle");
        }
        handle_ = handle;
        own_ = own;
    }

    ~FFI()
    {
        if (own_) {
            rnp_ffi_destroy(handle_);
        }
    }

    std::unique_ptr<Key>
    locate_key(const std::string &id_type, const std::string &id)
    {
        rnp_key_handle_t handle = NULL;
        if (rnp_locate_key(handle_, id_type.c_str(), id.c_str(), &handle)) {
            return nullptr;
        }
        auto res = new (std::nothrow) Key(handle);
        if (!res) {
            rnp_key_handle_destroy(handle);
        }
        return std::unique_ptr<Key>(res);
    }

    std::unique_ptr<IdentifierIterator>
    iterator_create(const std::string &it_type)
    {
        rnp_identifier_iterator_t it = NULL;
        if (rnp_identifier_iterator_create(handle_, &it, it_type.c_str())) {
            return nullptr;
        }
        auto res = new (std::nothrow) IdentifierIterator(it);
        if (!res) {
            rnp_identifier_iterator_destroy(it);
        }
        return std::unique_ptr<IdentifierIterator>(res);
    }

    bool
    request_password(Key &key, const std::string &ctx, String &password)
    {
        return !rnp_request_password(handle_, key.handle(), ctx.c_str(), password.set());
    }
};
} // namespace rnpffi

#endif /* RNPCPP_HPP_ */
