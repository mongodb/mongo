#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
unpack_prle_table_t px_unpack_prle_table = {
	px_qplc_unpack_prle_8u,
	px_qplc_unpack_prle_16u,
	px_qplc_unpack_prle_32u};
}
