/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#include "qpl/c_api/defs.h"
#include "common/defs.hpp"
#include "util/checkers.hpp"
#include "compression/dictionary/dictionary_defs.hpp"
#include "compression/dictionary/dictionary_utils.hpp"

extern "C" {

size_t qpl_get_dictionary_size(sw_compression_level sw_level, hw_compression_level hw_level, size_t raw_dict_size) {
    using namespace qpl::ml;
    return compression::get_dictionary_size(static_cast<software_compression_level>(sw_level),
                                            static_cast<hardware_compression_level>(hw_level),
                                            raw_dict_size);
}

 qpl_status qpl_get_existing_dict_size(qpl_dictionary *dict_ptr, size_t *destination) {
    using namespace qpl::ml;
    auto status = qpl::ml::bad_argument::check_for_nullptr(dict_ptr, destination);

    if (status != status_list::ok) {
        return static_cast<qpl_status>(status);
    }

    *destination = compression::get_dictionary_size(dict_ptr->sw_level,
                                                    dict_ptr->hw_level,
                                                    dict_ptr->raw_dictionary_size);

    return static_cast<qpl_status>(status);
}

qpl_status qpl_build_dictionary(qpl_dictionary *dict_ptr,
                                sw_compression_level sw_level,
                                hw_compression_level hw_level,
                                const uint8_t *raw_dict_ptr,
                                size_t raw_dict_size) {
    using namespace qpl::ml;
    auto status = qpl::ml::bad_argument::check_for_nullptr(dict_ptr, raw_dict_ptr);

    if (status != status_list::ok) {
        return static_cast<qpl_status>(status);
    }

    status =  compression::build_dictionary(*dict_ptr,
                                            static_cast<software_compression_level>(sw_level),
                                            static_cast<hardware_compression_level>(hw_level),
                                            raw_dict_ptr,
                                            raw_dict_size);
    return static_cast<qpl_status>(status);
}

qpl_status qpl_set_dictionary_id(qpl_dictionary *dictionary_ptr, uint32_t dictionary_id) {
    using namespace qpl::ml;
    auto status = qpl::ml::bad_argument::check_for_nullptr(dictionary_ptr);

    if (status != status_list::ok) {
        return static_cast<qpl_status>(status);
    }

    dictionary_ptr->dictionary_id = dictionary_id;
    return QPL_STS_OK;
}

qpl_status qpl_get_dictionary_id(qpl_dictionary *dictionary_ptr, uint32_t *destination) {
    using namespace qpl::ml;
    auto status = qpl::ml::bad_argument::check_for_nullptr(dictionary_ptr, destination);

    if (status != status_list::ok) {
        return static_cast<qpl_status>(status);
    }

    *destination = dictionary_ptr->dictionary_id;
    return QPL_STS_OK;
}

}
