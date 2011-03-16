/* DO NOT EDIT: automatically built by dist/log.py. */

static inline int
__wt_logput_debug(SESSION *session, const char * message)
{
	return (__wt_log_put(session, &__wt_logdesc_debug, message));
}
