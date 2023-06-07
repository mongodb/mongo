#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
memory_copy_table_t px_memory_copy_table = {
	px_qplc_copy_8u,
	px_qplc_copy_16u,
	px_qplc_copy_32u};
}
