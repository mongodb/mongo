#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
memory_copy_table_t avx512_memory_copy_table = {
	avx512_qplc_copy_8u,
	avx512_qplc_copy_16u,
	avx512_qplc_copy_32u};
}
