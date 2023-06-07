#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
unpack_prle_table_t avx512_unpack_prle_table = {
	avx512_qplc_unpack_prle_8u,
	avx512_qplc_unpack_prle_16u,
	avx512_qplc_unpack_prle_32u};
}
