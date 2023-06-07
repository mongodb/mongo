#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
move_table_t avx512_move_table = {
	avx512_qplc_move_8u};
}
