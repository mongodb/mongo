#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
pack_index_table_t px_pack_index_table = {
	px_qplc_pack_bits_nu,
	px_qplc_pack_index_8u,
	px_qplc_pack_index_8u16u,
	px_qplc_pack_index_8u32u,
	px_qplc_pack_bits_be_nu,
	px_qplc_pack_index_8u,
	px_qplc_pack_index_be_8u16u,
	px_qplc_pack_index_be_8u32u};
}
