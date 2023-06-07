#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
select_table_t px_select_table = {
	px_qplc_select_8u,
	px_qplc_select_16u,
	px_qplc_select_32u};
}
