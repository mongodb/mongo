/*
 * Copyright (c) 2017-2020 [Ribose Inc](https://www.ribose.com).
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

#ifndef STREAM_PACKET_H_
#define STREAM_PACKET_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "types.h"
#include "stream-common.h"
#include "enc_material.hpp"
#include "fingerprint.hpp"

/* maximum size of the 'small' packet */
#define PGP_MAX_PKT_SIZE 0x100000

/* maximum size of indeterminate-size packet allowed with old length format */
#define PGP_MAX_OLD_LEN_INDETERMINATE_PKT_SIZE 0x40000000

typedef struct pgp_packet_hdr_t {
    pgp_pkt_type_t tag;                      /* packet tag */
    uint8_t        hdr[PGP_MAX_HEADER_SIZE]; /* PGP packet header, needed for AEAD */
    size_t         hdr_len;                  /* length of the header */
    size_t         pkt_len;       /* packet body length if non-partial and non-indeterminate */
    bool           partial;       /* partial length packet */
    bool           indeterminate; /* indeterminate length packet */
} pgp_packet_hdr_t;

/* structure for convenient writing or parsing of non-stream packets */
typedef struct pgp_packet_body_t {
  private:
    pgp_pkt_type_t       tag_;  /* packet tag */
    std::vector<uint8_t> data_; /* packet bytes */
    /* fields below are filled only for parsed packet */
    uint8_t hdr_[PGP_MAX_HEADER_SIZE]{}; /* packet header bytes */
    size_t  hdr_len_{};                  /* number of bytes in hdr */
    size_t  pos_{};                      /* current read position in packet data */
    bool secure_{}; /* contents of the packet are secure so must be wiped in the destructor */
  public:
    /** @brief initialize writing of packet body
     *  @param tag tag of the packet
     **/
    pgp_packet_body_t(pgp_pkt_type_t tag);
    /** @brief init packet body (without headers) with memory. Used for easier data parsing.
     *  @param data buffer with packet body part
     *  @param len number of available bytes in mem
     */
    pgp_packet_body_t(const uint8_t *data, size_t len);
    pgp_packet_body_t(const std::vector<uint8_t> &data);

    pgp_packet_body_t(const pgp_packet_body_t &src) = delete;
    pgp_packet_body_t(pgp_packet_body_t &&src) = delete;
    pgp_packet_body_t &operator=(const pgp_packet_body_t &) = delete;
    pgp_packet_body_t &operator=(pgp_packet_body_t &&) = delete;
    ~pgp_packet_body_t();

    /** @brief pointer to the data, kept in the packet */
    uint8_t *data() noexcept;
    /** @brief pointer to the current data pointer */
    uint8_t *cur() noexcept;
    /** @brief number of bytes, kept in the packet (without the header) */
    size_t size() const noexcept;
    /** @brief number of bytes left to read */
    size_t left() const noexcept;
    /** @brief skip bytes in packet body */
    void skip(size_t bt) noexcept;
    void skip_back(size_t bt) noexcept;
    /** @brief get next byte from the packet body, populated with read() call.
     *  @param val result will be stored here on success
     *  @return true on success or false otherwise (if end of the packet is reached)
     **/
    bool get(uint8_t &val) noexcept;
    /** @brief get next big-endian uint16 from the packet body, populated with read() call.
     *  @param val result will be stored here on success
     *  @return true on success or false otherwise (if end of the packet is reached)
     **/
    bool get(uint16_t &val) noexcept;
    /** @brief get next big-endian uint32 from the packet body, populated with read() call.
     *  @param val result will be stored here on success
     *  @return true on success or false otherwise (if end of the packet is reached)
     **/
    bool get(uint32_t &val) noexcept;
    /** @brief get some bytes from the packet body, populated with read() call.
     *  @param val packet body bytes will be stored here. Must be capable of storing len bytes.
     *  @param len number of bytes to read
     *  @return true on success or false otherwise (if end of the packet is reached)
     **/
    bool get(uint8_t *val, size_t len) noexcept;
    /**
     * @brief Get some bytes of data to vector, resizing it accordingly.
     *
     * @param len number of bytes to read.
     */
    bool get(std::vector<uint8_t> &val, size_t len);
    /** @brief get next keyid from the packet body, populated with read() call.
     *  @param val result will be stored here on success
     *  @return true on success or false otherwise (if end of the packet is reached)
     **/
    bool get(pgp::KeyID &val) noexcept;
    /** @brief get next mpi from the packet body, populated with read() call.
     *  @param val result will be stored here on success
     *  @return true on success or false otherwise (if end of the packet is reached
     *          or mpi is ill-formed)
     **/
    bool get(pgp::mpi &val) noexcept;
    /** @brief Read ECC key curve and convert it to pgp_curve_t */
    bool get(pgp_curve_t &val) noexcept;
    /** @brief read s2k from the packet */
    bool get(pgp_s2k_t &s2k) noexcept;
    /** @brief append some bytes to the packet body */
    void add(const void *data, size_t len);
    /** @brief append some bytes to the packet body */
    void add(const std::vector<uint8_t> &data);
    /** @brief append single byte to the packet body */
    void add_byte(uint8_t bt);
    /** @brief append big endian 16-bit value to the packet body */
    void add_uint16(uint16_t val);
    /** @brief append big endian 32-bit value to the packet body */
    void add_uint32(uint32_t val);
    /** @brief append keyid to the packet body */
    void add(const pgp::KeyID &val);
    /** @brief add pgp mpi (including header) to the packet body */
    void add(const pgp::mpi &val);
    /**
     * @brief add pgp signature subpackets (including their length) to the packet body
     * @param sig signature, containing subpackets
     * @param hashed whether write hashed or not hashed subpackets
     */
    void add_subpackets(const pgp::pkt::Signature &sig, bool hashed);
    /** @brief add ec curve description to the packet body */
    void add(const pgp_curve_t curve);
    /** @brief add s2k description to the packet body */
    void add(const pgp_s2k_t &s2k);
    /** @brief read 'short-length' packet body (including tag and length bytes) from the source
     *  @param src source to read from
     *  @return RNP_SUCCESS or error code if operation failed
     **/
    rnp_result_t read(pgp_source_t &src) noexcept;
    /** @brief write packet header, length and body to the dst
     *  @param dst destination to write to.
     *  @param hdr write packet's header or not
     **/
    void write(pgp_dest_t &dst, bool hdr = true) noexcept;
    /** @brief mark contents as secure, so secure_clear() must be called in the destructor */
    void mark_secure(bool secure = true) noexcept;
} pgp_packet_body_t;

/** public-key encrypted session key packet */
typedef struct pgp_pk_sesskey_t {
    pgp_pkesk_version_t  version{};
    pgp_pubkey_alg_t     alg{};
    std::vector<uint8_t> material_buf{};

    /* v3 PKESK */
    pgp::KeyID     key_id{};
    pgp_symm_alg_t salg;

#if defined(ENABLE_CRYPTO_REFRESH)
    /* v6 PKESK */
    pgp::Fingerprint fp;
#endif

    void         write(pgp_dest_t &dst) const;
    rnp_result_t parse(pgp_source_t &src);
    /**
     * @brief Parse encrypted material which is stored in packet in raw.
     * @return Parsed material or nullptr. May also throw an exception.
     */
    std::unique_ptr<pgp::EncMaterial> parse_material() const;
    /**
     * @brief Write encrypted material to the material_buf.
     * @param material populated encrypted material.
     */
    void write_material(const pgp::EncMaterial &material);
} pgp_pk_sesskey_t;

/** pkp_sk_sesskey_t */
typedef struct pgp_sk_sesskey_t {
    unsigned       version{};
    pgp_symm_alg_t alg{};
    pgp_s2k_t      s2k{};
    uint8_t        enckey[PGP_MAX_KEY_SIZE + PGP_AEAD_MAX_TAG_LEN + 1]{};
    unsigned       enckeylen{};
    /* v5/v6 specific fields */
    pgp_aead_alg_t aalg{};
    uint8_t        iv[PGP_MAX_BLOCK_SIZE]{};
    unsigned       ivlen{};

    void         write(pgp_dest_t &dst) const;
    rnp_result_t parse(pgp_source_t &src);
} pgp_sk_sesskey_t;

/** pgp_one_pass_sig_t */
typedef struct pgp_one_pass_sig_t {
    uint8_t          version{};
    pgp_sig_type_t   type{};
    pgp_hash_alg_t   halg{};
    pgp_pubkey_alg_t palg{};
    pgp::KeyID       keyid{};
    unsigned         nested{};

    void         write(pgp_dest_t &dst) const;
    rnp_result_t parse(pgp_source_t &src);
} pgp_one_pass_sig_t;

/** Struct to hold userid or userattr packet. We don't parse userattr now, just storing the
 *  binary blob as it is. It may be distinguished by tag field.
 */
typedef struct pgp_userid_pkt_t {
    pgp_pkt_type_t       tag;
    std::vector<uint8_t> uid;

    bool operator==(const pgp_userid_pkt_t &src) const;
    bool operator!=(const pgp_userid_pkt_t &src) const;
    pgp_userid_pkt_t() : tag(PGP_PKT_RESERVED){};

    void         write(pgp_dest_t &dst) const;
    rnp_result_t parse(pgp_source_t &src);
} pgp_userid_pkt_t;

/** @brief write new packet length
 *  @param buf pre-allocated buffer, must have 5 bytes
 *  @param len packet length
 *  @return number of bytes, saved in buf
 **/
size_t write_packet_len(uint8_t *buf, size_t len);

/** @brief get packet type from the packet header byte
 *  @param ptag first byte of the packet header
 *  @return packet type or -1 if ptag is wrong
 **/
int get_packet_type(uint8_t ptag);

/** @brief peek the packet type from the stream
 *  @param src source to peek from
 *  @return packet tag or -1 if read failed or packet header is malformed
 */
int stream_pkt_type(pgp_source_t &src);

/** @brief Peek length of the packet header. Returns false on error.
 *  @param src source to read length from
 *  @param hdrlen header length will be put here on success. Cannot be NULL.
 *  @return true on success or false if there is a read error or packet length
 *          is ill-formed
 **/
bool stream_pkt_hdr_len(pgp_source_t &src, size_t &hdrlen);

bool stream_old_indeterminate_pkt_len(pgp_source_t *src);

bool stream_partial_pkt_len(pgp_source_t *src);

size_t get_partial_pkt_len(uint8_t blen);

/** @brief Read packet length for fixed-size (say, small) packet. Returns false on error.
 *  Will also read packet tag byte. We do not allow partial length here as well as large
 *  packets (so ignoring possible size_t overflow)
 *
 *  @param src source to read length from
 *  @param pktlen packet length will be stored here on success. Cannot be NULL.
 *  @return true on success or false if there is read error or packet length is ill-formed
 **/
bool stream_read_pkt_len(pgp_source_t &src, size_t *pktlen);

/** @brief Read partial packet chunk length.
 *
 *  @param src source to read length from
 *  @param clen chunk length will be stored here on success. Cannot be NULL.
 *  @param last will be set to true if chunk is last (i.e. has non-partial length)
 *  @return true on success or false if there is read error or packet length is ill-formed
 **/
bool stream_read_partial_chunk_len(pgp_source_t *src, size_t *clen, bool *last);

/** @brief get and parse OpenPGP packet header to the structure.
 *         Note: this will not read but just peek required bytes.
 *
 *  @param src source to read from
 *  @param hdr header structure
 *  @return RNP_SUCCESS or error code if operation failed
 **/
rnp_result_t stream_peek_packet_hdr(pgp_source_t *src, pgp_packet_hdr_t *hdr);

/* Packet handling functions */

/** @brief read OpenPGP packet from the stream, and write its contents to another stream.
 *  @param src source with packet data
 *  @param dst destination to write packet contents. All write failures on dst
 *             will be ignored. Can be NULL if you need just to skip packet.
 *  @return RNP_SUCCESS or error code if operation failed.
 */
rnp_result_t stream_read_packet(pgp_source_t *src, pgp_dest_t *dst);

rnp_result_t stream_skip_packet(pgp_source_t *src);

rnp_result_t stream_parse_marker(pgp_source_t &src);

/* Public/Private key or Subkey */

bool is_key_pkt(int tag);

bool is_subkey_pkt(int tag);

bool is_primary_key_pkt(int tag);

bool is_public_key_pkt(int tag);

bool is_secret_key_pkt(int tag);

bool is_rsa_key_alg(pgp_pubkey_alg_t alg);

#endif
