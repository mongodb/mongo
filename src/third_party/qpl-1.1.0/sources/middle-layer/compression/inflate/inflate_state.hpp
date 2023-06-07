/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/


#ifndef QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_CONTAINERS_DECOMPRESSION_STATE_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_CONTAINERS_DECOMPRESSION_STATE_HPP_

#include "util/memory.hpp"
#include "common/linear_allocator.hpp"
#include "compression/dictionary/dictionary_utils.hpp"
#include "inflate.hpp"
#include "inflate_defs.hpp"
#include "hw_aecs_api.h"
#include "hw_descriptors_api.h"
#include "compression/multitask/multi_task.hpp"
#include "compression/utils.hpp"

namespace qpl::ml::compression {
template <>
class inflate_state<execution_path_t::software> {
public:
    template<bool safe_creation = false>
    static auto create(const util::linear_allocator &allocator) noexcept -> inflate_state {
        auto state = inflate_state<execution_path_t::software>(allocator);
        if constexpr (safe_creation) {
            state.reset();
        }
        state.processing_step = util::multitask_status::multi_chunk_first_chunk;

        return state;
    }

    static auto restore(const util::linear_allocator &allocator) noexcept -> inflate_state {
        return inflate_state<execution_path_t::software>(allocator);
    };

    [[nodiscard]] static inline auto get_buffer_size() noexcept -> uint32_t {
        size_t size = 0;
        size += sizeof(isal_inflate_state);

        return static_cast<uint32_t>(util::align_size(size, 1_kb));
    }

    template <class iterator_t>
    inline auto output(iterator_t begin, iterator_t end) noexcept -> inflate_state &;

    template <class iterator_t>
    inline auto input(iterator_t begin, iterator_t end) noexcept -> inflate_state &;

    inline auto decompress_table(decompression_huffman_table &table) noexcept -> inflate_state &;

    inline auto dictionary(qpl_dictionary &dictionary) noexcept -> inflate_state &;

    inline auto input_access(access_properties properties) noexcept -> inflate_state &;

    inline auto crc_seed(uint32_t seed) noexcept -> inflate_state &;

    inline auto terminate() noexcept -> inflate_state &;

    inline auto in_progress() noexcept -> inflate_state &;

    inline auto flush_out() noexcept -> inflate_state &;

    [[nodiscard]] inline auto is_first() const noexcept -> bool;

    [[nodiscard]] inline auto is_last() const noexcept -> bool;

    [[nodiscard]] inline auto is_final() const noexcept -> bool;

    [[nodiscard]] inline auto is_dictionary_available() const noexcept -> bool;

    [[nodiscard]] inline auto get_input_data() const noexcept -> uint8_t *;

    [[nodiscard]] inline auto get_input_size() const noexcept -> uint32_t;

    [[nodiscard]] inline auto get_output_data() const noexcept -> uint8_t *;

    [[nodiscard]] inline auto get_crc() const noexcept -> uint32_t;

    [[nodiscard]] inline auto build_state() -> isal_inflate_state *;

    [[nodiscard]] inline auto get_state() -> isal_inflate_state *;

    [[nodiscard]] inline auto get_dictionary() -> qpl_dictionary  *;

    static constexpr auto execution_path = execution_path_t::software;

    // todo make private
    access_properties access_properties_ = {false, 0u, 0u};

    inflate_state() noexcept = default;

private:
    isal_inflate_state     *inflate_state_   = nullptr;
    util::multitask_status processing_step   = util::multitask_status::ready;
    bool                   is_dictionary_set = false;
    qpl_dictionary         *dictionary_ptr   = nullptr;

    explicit inflate_state(const util::linear_allocator &allocator) {
        inflate_state_ = allocator.allocate<isal_inflate_state, util::memory_block_t::not_aligned>(1u);
    };


    /**
     * @brief Hard resetting method which performs complex partial inflate_state zeroing.
     *
     */
    inline void reset() noexcept {
        reset_inflate_state(inflate_state_);
    }

    /**
     * @brief Soft resetting method which performs partial inflate_state zeroing.
     *
     */

    inline void soft_reset() noexcept {
        inflate_state_->wrapper_flag      = 0;
        inflate_state_->read_in           = 0;
        inflate_state_->read_in_length    = 0;
        inflate_state_->tmp_out_valid     = 0;
        inflate_state_->tmp_out_processed = 0;
    }

    inline void inflate_state_skip_start_bits(uint32_t number_of_bits, bool force_load) noexcept {
        if ((0u != number_of_bits || force_load) && (0u < inflate_state_->avail_in)) {
            // Load new byte to ISA-L inflate state
            inflate_state_->read_in = *(inflate_state_->next_in++);
            inflate_state_->avail_in--;
            inflate_state_->read_in_length = 8u;

            // Fulfill inflate byte buffer
            while (inflate_state_->read_in_length < 64 &&
                   inflate_state_->avail_in > 1u) {
                inflate_state_->read_in |=
                        ((uint64_t) *(inflate_state_->next_in++)) << (uint32_t) (inflate_state_->read_in_length);
                inflate_state_->avail_in--;
                inflate_state_->read_in_length += 8u;
            }

            // Update actual number of bits
            inflate_state_->read_in >>= number_of_bits;
            inflate_state_->read_in_length -= number_of_bits;
        }
    }
};

template <>
class inflate_state<execution_path_t::hardware> {
public:
    template<bool safe_creation = false>
    static auto create(const util::linear_allocator &allocator) noexcept -> inflate_state {
        auto state = inflate_state<execution_path_t::hardware>(allocator);
        if constexpr (safe_creation) {
            state.reset();
        }
        state.processing_step                     = util::multitask_status::multi_chunk_first_chunk;
        state.execution_state_ptr->aecs_index     = 0u;
        state.decompression_state_ptr->block_type = deflate_block_type_e::undefined;

        return state;
    }

    static auto restore(const util::linear_allocator &allocator) noexcept -> inflate_state {
        return inflate_state<execution_path_t::hardware>(allocator);
    };

    ~inflate_state() {
        this->execution_state_ptr->aecs_index ^= 1u;
    }

    [[nodiscard]] static constexpr inline auto get_buffer_size() noexcept -> uint32_t {
        size_t size = 0;
        size += sizeof(execution_state);
        size += sizeof(decompression_state_t);
        size += util::align_size(sizeof(hw_descriptor));
        size += util::align_size(sizeof(HW_PATH_VOLATILE hw_completion_record));
        size += util::align_size(sizeof(hw_iaa_aecs_analytic) * 2);

        return static_cast<uint32_t>(util::align_size(size, 1_kb));
    }

    template <class iterator_t>
    inline auto output(iterator_t begin, iterator_t end) noexcept -> inflate_state &;

    template <class iterator_t>
    inline auto input(iterator_t begin, iterator_t end) noexcept -> inflate_state &;

    inline auto decompress_table(decompression_huffman_table &table) noexcept -> inflate_state &;

    inline auto crc_seed(uint32_t seed) noexcept -> inflate_state &;

    inline auto input_access(access_properties properties) noexcept -> inflate_state &;

    inline auto stop_check_condition(end_processing_condition_t end_processing_condition) noexcept -> inflate_state &;

    inline auto terminate() noexcept -> inflate_state &;

    inline auto flush_out() noexcept -> inflate_state &;

    inline auto dictionary(qpl_dictionary &dictionary) noexcept -> inflate_state &;

    [[nodiscard]] inline auto is_first() const noexcept -> bool;

    [[nodiscard]] inline auto is_last() const noexcept -> bool;

    [[nodiscard]] inline auto get_input_data() const noexcept -> uint8_t *;

    [[nodiscard]] inline auto get_input_size() const noexcept -> uint32_t;

    [[nodiscard]] inline auto get_output_data() const noexcept -> uint8_t *;

    [[nodiscard]] inline auto get_crc() const noexcept -> uint32_t;

    template <inflate_mode_t mode>
    [[nodiscard]] inline auto build_descriptor() noexcept -> hw_descriptor *;

    [[nodiscard]] inline auto handler() const noexcept -> HW_PATH_VOLATILE hw_completion_record *;

    static constexpr auto execution_path = execution_path_t::hardware;

private:

    friend auto
    inflate<execution_path, inflate_mode_t::inflate_header>(inflate_state<execution_path> &decompression_state,
                                                            end_processing_condition_t end_processing_condition) noexcept -> decompression_operation_result_t;

    [[nodiscard]] inline auto get_current_block_type() const noexcept -> deflate_block_type_e;

    inline void set_block_type(deflate_block_type_e block_type) noexcept;

    hw_descriptor        *descriptor_                         = nullptr;
    HW_PATH_VOLATILE hw_completion_record *completion_record_ = nullptr;
    hw_iaa_aecs_analytic                  *decompress_aecs_   = nullptr;

    bool                   is_dictionary_set = false;
    qpl_dictionary         *dictionary_ptr   = nullptr;

    const util::linear_allocator &allocator_;

    struct {
        uint8_t  *next_out = nullptr;
        uint32_t avail_out = 0u;
        uint8_t  *next_in  = nullptr;
        uint32_t avail_in  = 0u;
        uint32_t crc       = 0u;
    } inflate_state_;

    bool flush_enabled_ = false;

    util::multitask_status     processing_step           = util::multitask_status::ready;
    access_properties          access_properties_        = {false, 0u, 0u};
    end_processing_condition_t end_processing_condition_ = stop_and_check_for_bfinal_eob;

    struct execution_state {
        uint8_t aecs_index;
    };

    execution_state *execution_state_ptr;

    struct decompression_state_t {
        deflate_block_type_e block_type;
    };

    decompression_state_t *decompression_state_ptr;

    explicit inflate_state(const util::linear_allocator &allocator) : allocator_(allocator) {
        execution_state_ptr     = allocator.allocate<execution_state>(1u);
        decompression_state_ptr = allocator.allocate<decompression_state_t>(1);
        descriptor_             = allocator.allocate<hw_descriptor, util::memory_block_t::aligned_64u>(1u);
        completion_record_      = allocator.allocate<hw_completion_record, util::memory_block_t::aligned_64u>(1u);
    };

    inline void initialize_random_access(hw_iaa_aecs_analytic *aecs_ptr,
                                         hw_descriptor *descriptor,
                                         uint8_t *source_ptr,
                                         uint32_t source_size,
                                         access_properties properties);

    // TODO: Replace full descriptor zeroing by partial zeroing
    inline void reset() noexcept {
        util::set_zeros(descriptor_, sizeof(hw_descriptor));
    }
};

/* ------ SOFTWARE STATE METHODS ------ */

template <class iterator_t>
inline auto inflate_state<execution_path_t::software>::output(iterator_t begin,
                                                              iterator_t end) noexcept -> inflate_state & {
    inflate_state_->next_out  = begin;
    inflate_state_->avail_out = static_cast<uint32_t>(std::distance(begin, end));

    return *this;
}

template <class iterator_t>
inline auto inflate_state<execution_path_t::software>::input(iterator_t begin,
                                                             iterator_t end) noexcept -> inflate_state & {
    inflate_state_->next_in  = begin;
    inflate_state_->avail_in = static_cast<uint32_t>(std::distance(begin, end));

    return *this;
}

inline auto inflate_state<execution_path_t::software>::input_access(access_properties properties) noexcept -> inflate_state & {
    access_properties_ = properties;
    if (access_properties_.is_random) {
        soft_reset();
    }

    return *this;
}

inline auto inflate_state<execution_path_t::software>::decompress_table(decompression_huffman_table &table) noexcept -> inflate_state & {
    // Obtain lookup table used to perform decompression with it
    auto *canned_table_ptr = table.get_canned_table();

    // Copy lookup tables from decompression table to inflate state
    auto *literal_table_ptr = reinterpret_cast<uint8_t *>(&canned_table_ptr->literal_huffman_codes);

    util::copy(literal_table_ptr,
               literal_table_ptr + sizeof(inflate_state_->lit_huff_code),
               reinterpret_cast<uint8_t *>(&inflate_state_->lit_huff_code));

    auto *distance_table_ptr = reinterpret_cast<uint8_t *>(&canned_table_ptr->distance_huffman_codes);

    util::copy(distance_table_ptr,
               distance_table_ptr + sizeof(inflate_state_->dist_huff_code),
               reinterpret_cast<uint8_t *>(&inflate_state_->dist_huff_code));

    inflate_state_->eob_code_and_len = canned_table_ptr->eob_code_and_len;
    inflate_state_->bfinal           = (canned_table_ptr->is_final_block) ? 1u : 0u;

    return *this;
}

inline auto inflate_state<execution_path_t::software>::dictionary(qpl_dictionary &dictionary) noexcept -> inflate_state & {
    auto status = isal_inflate_set_dict(inflate_state_,
                                        get_dictionary_data(dictionary),
                                        static_cast<uint32_t>(dictionary.raw_dictionary_size));

    if (!status) {
        this->is_dictionary_set = true;
        dictionary_ptr = &dictionary;
    }

    return *this;
}

inline auto inflate_state<execution_path_t::software>::crc_seed(uint32_t seed) noexcept -> inflate_state & {
    inflate_state_->crc = seed;

    return *this;
}

inline auto inflate_state<execution_path_t::software>::terminate() noexcept -> inflate_state & {
    processing_step = static_cast<util::multitask_status>(processing_step
                                                          | util::multitask_status::multi_chunk_last_chunk);

    return *this;
}

/**< @todo Change logic of setting processing step */
inline auto inflate_state<execution_path_t::software>::in_progress() noexcept -> inflate_state &{
    if (util::multitask_status::multi_chunk_first_chunk == processing_step) {
        processing_step = util::multitask_status::multi_chunk_in_progress;
    }

    return *this;
}

inline auto inflate_state<execution_path_t::software>::flush_out() noexcept -> inflate_state & {
    return *this;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::software>::is_first() const noexcept -> bool {
    return processing_step & util::multitask_status::multi_chunk_first_chunk;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::software>::is_last() const noexcept -> bool {
    return processing_step & util::multitask_status::multi_chunk_last_chunk;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::software>::is_final() const noexcept -> bool {
    return (processing_step & util::multitask_status::multi_chunk_last_chunk) ||
           (processing_step & util::multitask_status::single_chunk_processing);
}


[[nodiscard]] inline auto inflate_state<execution_path_t::software>::is_dictionary_available() const noexcept -> bool {
    return is_dictionary_set;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::software>::get_input_data() const noexcept -> uint8_t * {
    return inflate_state_->next_in;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::software>::get_input_size() const noexcept -> uint32_t {
    return inflate_state_->avail_in;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::software>::get_output_data() const noexcept -> uint8_t * {
    return inflate_state_->next_out;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::software>::get_crc() const noexcept -> uint32_t {
    return inflate_state_->crc;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::software>::build_state() -> isal_inflate_state * {
    inflate_state_skip_start_bits(access_properties_.ignore_start_bits,
                                  access_properties_.is_random);

    if (is_first()) {
        inflate_state_->block_state = ISAL_BLOCK_NEW_HDR;
    }

    if (inflate_state_->block_state == ISAL_BLOCK_FINISH) {
        inflate_state_->block_state = access_properties_.is_random ? ISAL_BLOCK_CODED : ISAL_BLOCK_NEW_HDR;
    }

    return inflate_state_;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::software>::get_state() -> isal_inflate_state * {
    return inflate_state_;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::software>::get_dictionary() -> qpl_dictionary  * {
    return dictionary_ptr;
}

/* ------ HARDWARE STATE METHODS ------ */
template <class iterator_t>
inline auto inflate_state<execution_path_t::hardware>::output(iterator_t begin,
                                                              iterator_t end) noexcept -> inflate_state & {
    inflate_state_.avail_out = static_cast<uint32_t>(std::distance(begin, end));
    inflate_state_.next_out  = reinterpret_cast<uint8_t *>(begin);

    hw_iaa_descriptor_set_output_buffer(descriptor_, inflate_state_.next_out, inflate_state_.avail_out);

    return *this;
}

template <class iterator_t>
inline auto inflate_state<execution_path_t::hardware>::input(iterator_t begin,
                                                             iterator_t end) noexcept -> inflate_state & {
    inflate_state_.next_in  = reinterpret_cast<uint8_t *>(begin);
    inflate_state_.avail_in = static_cast<uint32_t>(std::distance(begin, end));

    hw_iaa_descriptor_set_input_buffer(descriptor_, inflate_state_.next_in, inflate_state_.avail_in);

    return *this;
}

inline auto inflate_state<execution_path_t::hardware>::input_access(access_properties properties) noexcept -> inflate_state & {
    access_properties_ = properties;

    return *this;
}

inline auto inflate_state<execution_path_t::hardware>::decompress_table(decompression_huffman_table &table) noexcept -> inflate_state & {
    decompress_aecs_ = reinterpret_cast<hw_iaa_aecs_analytic *>(table.get_hw_decompression_state());

    return *this;
}

inline auto inflate_state<execution_path_t::hardware>::crc_seed(uint32_t seed) noexcept -> inflate_state & {
    inflate_state_.crc = seed;

    return *this;
}

inline auto inflate_state<execution_path_t::hardware>::stop_check_condition(end_processing_condition_t end_processing_condition) noexcept -> inflate_state & {
    end_processing_condition_ = end_processing_condition;

    return *this;
}

inline auto inflate_state<execution_path_t::hardware>::terminate() noexcept -> inflate_state & {
    processing_step = static_cast<util::multitask_status>(processing_step
                                                          | util::multitask_status::multi_chunk_last_chunk);

    return *this;
}

inline auto inflate_state<execution_path_t::hardware>::flush_out() noexcept -> inflate_state & {
    flush_enabled_ = true;

    return *this;
}

inline auto inflate_state<execution_path_t::hardware>::dictionary(qpl_dictionary &dictionary) noexcept -> inflate_state & {
    this->is_dictionary_set = true;
    dictionary_ptr          = &dictionary;
    return *this;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::is_first() const noexcept -> bool {
    return processing_step & util::multitask_status::multi_chunk_first_chunk;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::is_last() const noexcept -> bool {
    return processing_step & util::multitask_status::multi_chunk_last_chunk;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::get_input_data() const noexcept -> uint8_t * {
    return inflate_state_.next_in;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::get_input_size() const noexcept -> uint32_t {
    return inflate_state_.avail_in;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::get_output_data() const noexcept -> uint8_t * {
    return inflate_state_.next_out;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::get_crc() const noexcept -> uint32_t {
    return inflate_state_.crc;
}

template <>
[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::build_descriptor<inflate_mode_t::inflate_default>() noexcept -> hw_descriptor * {
    decompress_aecs_ = allocator_.allocate<hw_iaa_aecs_analytic, util::memory_block_t::aligned_64u>(2u);

    auto access_policy = static_cast<hw_iaa_aecs_access_policy>(util::aecs_decompress_access_lookup_table[processing_step] |
                                                                execution_state_ptr->aecs_index);

    if (is_dictionary_set) {
        hw_iaa_aecs_decompress_set_dictionary(&decompress_aecs_[execution_state_ptr->aecs_index].inflate_options,
                                              get_dictionary_data(*dictionary_ptr),
                                              dictionary_ptr->raw_dictionary_size);

        hw_iaa_aecs_decompress_set_decompression_state(&decompress_aecs_[execution_state_ptr->aecs_index].inflate_options,
                                                       hw_iaa_aecs_decompress_state::hw_aecs_at_start_block_header);

        if (processing_step == util::multitask_status::multi_chunk_first_chunk) {
            // First chunk of multi_chunk mode, always read and always write AECS
            access_policy = static_cast<hw_iaa_aecs_access_policy>(util::aecs_decompress_access_lookup_table[util::multitask_status::ready] |
                                                                   execution_state_ptr->aecs_index);
        } else {
            // Stateless mode, always read and maybe write AECS (only in case of overflow)
            access_policy = static_cast<hw_iaa_aecs_access_policy>(util::aecs_decompress_access_lookup_table[util::multitask_status::multi_chunk_last_chunk] |
                                                                   execution_state_ptr->aecs_index);
        }


        is_dictionary_set = false;
    }

    hw_iaa_aecs_decompress_set_crc_seed(&decompress_aecs_[execution_state_ptr->aecs_index], inflate_state_.crc);
    hw_iaa_descriptor_init_inflate(descriptor_, decompress_aecs_, HW_AECS_ANALYTICS_SIZE, access_policy);
    hw_iaa_descriptor_set_inflate_stop_check_rule(descriptor_,
                                                  static_cast<hw_iaa_decompress_start_stop_rule_t>(end_processing_condition_),
                                                  is_last() || (end_processing_condition_ & check_on_nonlast_block));
    hw_iaa_descriptor_set_completion_record(descriptor_, completion_record_);

    if (flush_enabled_) {
        hw_iaa_descriptor_inflate_set_flush(descriptor_);
    }

    return descriptor_;
}

template <>
[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::build_descriptor<inflate_mode_t::inflate_header>() noexcept -> hw_descriptor * {
    auto *buffer_ptr = allocator_.allocate<uint8_t, util::memory_block_t::aligned_64u>(
            HW_AECS_ANALYTIC_RANDOM_ACCESS_SIZE * 2);
    decompress_aecs_ = reinterpret_cast<hw_iaa_aecs_analytic *>(buffer_ptr);

    auto aecs_policy = hw_aecs_toggle_rw;

    auto *header_aecs_ptr = reinterpret_cast<hw_iaa_aecs_analytic *>(buffer_ptr + HW_AECS_ANALYTIC_RANDOM_ACCESS_SIZE);
    hw_iaa_aecs_decompress_clean_input_accumulator(&header_aecs_ptr->inflate_options);

    if (0u != access_properties_.ignore_start_bits) {
        util::set_zeros(reinterpret_cast<uint8_t *>(header_aecs_ptr), HW_AECS_ANALYTIC_RANDOM_ACCESS_SIZE);
        aecs_policy = static_cast<hw_iaa_aecs_access_policy>(hw_aecs_access_read | hw_aecs_toggle_rw);

        initialize_random_access(header_aecs_ptr,
                                 descriptor_,
                                 inflate_state_.next_in,
                                 inflate_state_.avail_in,
                                 access_properties_);
    }

    hw_iaa_descriptor_init_inflate_header(descriptor_,
                                          decompress_aecs_,
                                          access_properties_.ignore_end_bits,
                                          aecs_policy);
    hw_iaa_descriptor_set_completion_record(descriptor_, completion_record_);

    return descriptor_;
}

template <>
[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::build_descriptor<inflate_mode_t::inflate_body>() noexcept -> hw_descriptor * {
    if (decompress_aecs_ == nullptr) {
        auto buffer = allocator_.allocate<uint8_t, util::memory_block_t::aligned_64u>(
                HW_AECS_ANALYTIC_RANDOM_ACCESS_SIZE);
        decompress_aecs_ = reinterpret_cast<hw_iaa_aecs_analytic *>(buffer);
    }

    hw_iaa_aecs_decompress_clean_input_accumulator(&decompress_aecs_->inflate_options);

    if (0u != access_properties_.ignore_start_bits) {
        initialize_random_access(decompress_aecs_,
                                 descriptor_,
                                 inflate_state_.next_in,
                                 inflate_state_.avail_in,
                                 access_properties_);
    }

    hw_iaa_aecs_decompress_set_crc_seed(decompress_aecs_, inflate_state_.crc);
    hw_iaa_descriptor_init_inflate_body(descriptor_,
                                        decompress_aecs_,
                                        access_properties_.ignore_end_bits);

    // Check if block type is known
    if (decompression_state_ptr->block_type != deflate_block_type_e::undefined) {
        constexpr uint16_t final_decompression_state_bit = 4;
        uint16_t decompression_state = (this->is_last()) ? final_decompression_state_bit : 0;

        if (decompression_state_ptr->block_type == deflate_block_type_e::coded) {
            decompression_state |= hw_iaa_aecs_decompress_state::hw_aecs_at_ll_token_non_final_block;
        } else {
            decompression_state |= hw_iaa_aecs_decompress_state::hw_aecs_at_stored_block_non_final_block;
        }

        hw_iaa_aecs_decompress_set_decompression_state(&decompress_aecs_->inflate_options,
                                                       static_cast<hw_iaa_aecs_decompress_state>(decompression_state));
    }

    hw_iaa_descriptor_set_completion_record(descriptor_, completion_record_);

    return descriptor_;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::handler() const noexcept -> HW_PATH_VOLATILE hw_completion_record * {
    return completion_record_;
}

[[nodiscard]] inline auto inflate_state<execution_path_t::hardware>::get_current_block_type() const noexcept -> deflate_block_type_e {
    if (decompress_aecs_) {
        const auto decompress_state = static_cast<hw_iaa_aecs_decompress_state>(decompress_aecs_->inflate_options.decompress_state);
        if (decompress_state == hw_iaa_aecs_decompress_state::hw_aecs_at_stored_block_non_final_block ||
            decompress_state == hw_iaa_aecs_decompress_state::hw_aecs_at_stored_block_final_block) {
            return deflate_block_type_e::stored;
        } else if (decompress_state == hw_iaa_aecs_decompress_state::hw_aecs_at_ll_token_non_final_block ||
                   decompress_state == hw_iaa_aecs_decompress_state::hw_aecs_at_ll_token_final_block) {
            return deflate_block_type_e::coded;
        }
    }

    return deflate_block_type_e::undefined;
}

inline void inflate_state<execution_path_t::hardware>::set_block_type(deflate_block_type_e block_type) noexcept {
    this->decompression_state_ptr->block_type = block_type;
}

inline void inflate_state<execution_path_t::hardware>::initialize_random_access(hw_iaa_aecs_analytic *const aecs_ptr,
                                                                                hw_descriptor *const descriptor,
                                                                                uint8_t *source_ptr,
                                                                                uint32_t source_size,
                                                                                access_properties properties) {
    aecs_ptr->inflate_options.idx_bit_offset = 7u & access_properties_.ignore_start_bits;

    hw_iaa_aecs_decompress_set_input_accumulator(&aecs_ptr->inflate_options,
                                                 source_ptr,
                                                 source_size,
                                                 properties.ignore_start_bits,
                                                 properties.ignore_end_bits);
    source_ptr++;
    source_size--;

    hw_iaa_descriptor_set_input_buffer(descriptor, source_ptr, source_size);
}

}

#endif //QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_CONTAINERS_DECOMPRESSION_STATE_HPP_
