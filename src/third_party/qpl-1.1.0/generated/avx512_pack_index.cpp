#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
pack_index_table_t avx512_pack_index_table = {
	avx512_qplc_pack_bits_nu,
	avx512_qplc_pack_index_8u,
	avx512_qplc_pack_index_8u16u,
	avx512_qplc_pack_index_8u32u,
	avx512_qplc_pack_bits_be_nu,
	avx512_qplc_pack_index_8u,
	avx512_qplc_pack_index_be_8u16u,
	avx512_qplc_pack_index_be_8u32u};
}
