#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
xor_checksum_table_t avx512_xor_checksum_table = {
	avx512_qplc_xor_checksum_8u};
}
