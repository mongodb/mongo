#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
expand_table_t px_expand_table = {
	px_qplc_expand_8u,
	px_qplc_expand_16u,
	px_qplc_expand_32u};
}
