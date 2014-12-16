/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int config_check(
    WT_SESSION_IMPL *, const WT_CONFIG_CHECK *, const char *, size_t);

/*
 * __conn_foc_add --
 *	Add a new entry into the connection's free-on-close list.
 */
static int
__conn_foc_add(WT_SESSION_IMPL *session, const void *p)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*
	 * Our caller is expected to be holding any locks we need.
	 */
	WT_RET(__wt_realloc_def(
	    session, &conn->foc_size, conn->foc_cnt + 1, &conn->foc));

	conn->foc[conn->foc_cnt++] = (void *)p;
	return (0);
}

/*
 * __wt_conn_foc_discard --
 *	Discard any memory the connection accumulated.
 */
void
__wt_conn_foc_discard(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	size_t i;

	conn = S2C(session);

	/*
	 * If we have a list of chunks to free, run through the list, then
	 * free the list itself.
	 */
	for (i = 0; i < conn->foc_cnt; ++i)
		__wt_free(session, conn->foc[i]);
	__wt_free(session, conn->foc);
}

/*
 * __wt_configure_method --
 *	WT_CONNECTION.configure_method.
 */
int
__wt_configure_method(WT_SESSION_IMPL *session,
    const char *method, const char *uri,
    const char *config, const char *type, const char *check)
{
	const WT_CONFIG_CHECK *cp;
	WT_CONFIG_CHECK *checks, *newcheck;
	const WT_CONFIG_ENTRY **epp;
	WT_CONFIG_ENTRY *entry;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	size_t cnt;
	char *newcheck_name, *p;

	/*
	 * !!!
	 * We ignore the specified uri, that is, all new configuration options
	 * will be valid for all data sources.   That shouldn't be too bad
	 * as the worst that can happen is an application might specify some
	 * configuration option and not get an error -- the option should be
	 * ignored by the underlying implementation since it's unexpected, so
	 * there shouldn't be any real problems.  Eventually I expect we will
	 * get the whole data-source thing sorted, at which time there may be
	 * configuration arrays for each data source, and that's when the uri
	 * will matter.
	 */
	WT_UNUSED(uri);

	conn = S2C(session);
	checks = newcheck = NULL;
	entry = NULL;
	newcheck_name = NULL;

	/* Argument checking; we only support a limited number of types. */
	if (config == NULL)
		WT_RET_MSG(session, EINVAL, "no configuration specified");
	if (type == NULL)
		WT_RET_MSG(session, EINVAL, "no configuration type specified");
	if (strcmp(type, "boolean") != 0 && strcmp(type, "int") != 0 &&
	    strcmp(type, "list") != 0 && strcmp(type, "string") != 0)
		WT_RET_MSG(session, EINVAL,
		    "type must be one of \"boolean\", \"int\", \"list\" or "
		    "\"string\"");

	/* Find a match for the method name. */
	for (epp = conn->config_entries; (*epp)->method != NULL; ++epp)
		if (strcmp((*epp)->method, method) == 0)
			break;
	if ((*epp)->method == NULL)
		WT_RET_MSG(session,
		    WT_NOTFOUND, "no method matching %s found", method);

	/*
	 * Technically possible for threads to race, lock the connection while
	 * adding the new configuration information.  We're holding the lock
	 * for an extended period of time, but configuration changes should be
	 * rare and only happen during startup.
	 */
	__wt_spin_lock(session, &conn->api_lock);

	/*
	 * Allocate new configuration entry and fill it in.
	 *
	 * The new base value is the previous base value, a separator and the
	 * new configuration string.
	 */
	WT_ERR(__wt_calloc_one(session, &entry));
	entry->method = (*epp)->method;
	WT_ERR(__wt_calloc_def(session,
	    strlen((*epp)->base) + strlen(",") + strlen(config) + 1, &p));
	(void)strcpy(p, (*epp)->base);
	(void)strcat(p, ",");
	(void)strcat(p, config);
	entry->base = p;

	/*
	 * There may be a default value in the config argument passed in (for
	 * example, (kvs_parallelism=64").  The default value isn't part of the
	 * name, build a new one.
	 */
	WT_ERR(__wt_strdup(session, config, &newcheck_name));
	if ((p = strchr(newcheck_name, '=')) != NULL)
		*p = '\0';

	/*
	 * The new configuration name may replace an existing check with new
	 * information, in that case skip the old version.
	 */
	cnt = 0;
	if ((*epp)->checks != NULL)
		for (cp = (*epp)->checks; cp->name != NULL; ++cp)
			++cnt;
	WT_ERR(__wt_calloc_def(session, cnt + 2, &checks));
	cnt = 0;
	if ((*epp)->checks != NULL)
		for (cp = (*epp)->checks; cp->name != NULL; ++cp)
			if (strcmp(newcheck_name, cp->name) != 0)
				checks[cnt++] = *cp;
	newcheck = &checks[cnt];
	newcheck->name = newcheck_name;
	WT_ERR(__wt_strdup(session, type, &newcheck->type));
	if (check != NULL)
		WT_ERR(__wt_strdup(session, check, &newcheck->checks));
	entry->checks = checks;

	/*
	 * Confirm the configuration string passes the new set of
	 * checks.
	 */
	WT_ERR(config_check(session, entry->checks, config, 0));

	/*
	 * The next time this configuration is updated, we don't want to figure
	 * out which of these pieces of memory were allocated and will need to
	 * be free'd on close (this isn't a heavily used API and it's too much
	 * work); add them all to the free-on-close list now.  We don't check
	 * for errors deliberately, we'd have to figure out which elements have
	 * already been added to the free-on-close array and which have not in
	 * order to avoid freeing chunks of memory twice.  Again, this isn't a
	 * commonly used API and it shouldn't ever happen, just leak it.
	 */
	(void)__conn_foc_add(session, entry->base);
	(void)__conn_foc_add(session, entry);
	(void)__conn_foc_add(session, checks);
	(void)__conn_foc_add(session, newcheck->type);
	(void)__conn_foc_add(session, newcheck->checks);
	(void)__conn_foc_add(session, newcheck_name);

	/*
	 * Instead of using locks to protect configuration information, assume
	 * we can atomically update a pointer to a chunk of memory, and because
	 * a pointer is never partially written, readers will correctly see the
	 * original or new versions of the memory.  Readers might be using the
	 * old version as it's being updated, though, which means we cannot free
	 * the old chunk of memory until all possible readers have finished.
	 * Currently, that's on connection close: in other words, we can use
	 * this because it's small amounts of memory, and we really, really do
	 * not want to acquire locks every time we access configuration strings,
	 * since that's done on every API call.
	 */
	WT_PUBLISH(*epp, entry);

	if (0) {
err:		if (entry != NULL) {
			__wt_free(session, entry->base);
			__wt_free(session, entry);
		}
		__wt_free(session, checks);
		if (newcheck != NULL) {
			__wt_free(session, newcheck->type);
			__wt_free(session, newcheck->checks);
		}
		__wt_free(session, newcheck_name);
	}

	__wt_spin_unlock(session, &conn->api_lock);
	return (ret);
}

/*
 * __wt_config_check --
 *	Check the keys in an application-supplied config string match what is
 *	specified in an array of check strings.
 */
int
__wt_config_check(WT_SESSION_IMPL *session,
    const WT_CONFIG_ENTRY *entry, const char *config, size_t config_len)
{
	/*
	 * Callers don't check, it's a fast call without a configuration or
	 * check array.
	 */
	return (config == NULL || entry->checks == NULL ?
	    0 : config_check(session, entry->checks, config, config_len));
}

/*
 * config_check --
 *	Check the keys in an application-supplied config string match what is
 * specified in an array of check strings.
 */
static int
config_check(WT_SESSION_IMPL *session,
    const WT_CONFIG_CHECK *checks, const char *config, size_t config_len)
{
	WT_CONFIG parser, cparser, sparser;
	WT_CONFIG_ITEM k, v, ck, cv, dummy;
	WT_DECL_RET;
	int badtype, found, i;

	/*
	 * The config_len parameter is optional, and allows passing in strings
	 * that are not nul-terminated.
	 */
	if (config_len == 0)
		WT_RET(__wt_config_init(session, &parser, config));
	else
		WT_RET(__wt_config_initn(session, &parser, config, config_len));
	while ((ret = __wt_config_next(&parser, &k, &v)) == 0) {
		if (k.type != WT_CONFIG_ITEM_STRING &&
		    k.type != WT_CONFIG_ITEM_ID)
			WT_RET_MSG(session, EINVAL,
			    "Invalid configuration key found: '%.*s'",
			    (int)k.len, k.str);

		/* Search for a matching entry. */
		for (i = 0; checks[i].name != NULL; i++)
			if (WT_STRING_MATCH(checks[i].name, k.str, k.len))
				break;
		if (checks[i].name == NULL)
			WT_RET_MSG(session, EINVAL,
			    "unknown configuration key: '%.*s'",
			    (int)k.len, k.str);

		if (strcmp(checks[i].type, "boolean") == 0) {
			badtype = (v.type != WT_CONFIG_ITEM_BOOL &&
			    (v.type != WT_CONFIG_ITEM_NUM ||
			    (v.val != 0 && v.val != 1)));
		} else if (strcmp(checks[i].type, "category") == 0) {
			/* Deal with categories of the form: XXX=(XXX=blah). */
			ret = config_check(session,
			    checks[i].subconfigs,
			    k.str + strlen(checks[i].name) + 1, v.len);
			if (ret != EINVAL)
				badtype = 0;
			else
				badtype = 1;
		} else if (strcmp(checks[i].type, "format") == 0) {
			badtype = 0;
		} else if (strcmp(checks[i].type, "int") == 0) {
			badtype = (v.type != WT_CONFIG_ITEM_NUM);
		} else if (strcmp(checks[i].type, "list") == 0) {
			badtype = (v.len > 0 &&
			    v.type != WT_CONFIG_ITEM_STRUCT);
		} else if (strcmp(checks[i].type, "string") == 0) {
			badtype = 0;
		} else
			WT_RET_MSG(session, EINVAL,
			    "unknown configuration type: '%s'",
			    checks[i].type);

		if (badtype)
			WT_RET_MSG(session, EINVAL,
			    "Invalid value for key '%.*s': expected a %s",
			    (int)k.len, k.str, checks[i].type);

		if (checks[i].checks == NULL)
			continue;

		/* Setup an iterator for the check string. */
		WT_RET(__wt_config_init(session, &cparser, checks[i].checks));
		while ((ret = __wt_config_next(&cparser, &ck, &cv)) == 0) {
			if (WT_STRING_MATCH("min", ck.str, ck.len)) {
				if (v.val < cv.val)
					WT_RET_MSG(session, EINVAL,
					    "Value too small for key '%.*s' "
					    "the minimum is %.*s",
					    (int)k.len, k.str,
					    (int)cv.len, cv.str);
			} else if (WT_STRING_MATCH("max", ck.str, ck.len)) {
				if (v.val > cv.val)
					WT_RET_MSG(session, EINVAL,
					    "Value too large for key '%.*s' "
					    "the maximum is %.*s",
					    (int)k.len, k.str,
					    (int)cv.len, cv.str);
			} else if (WT_STRING_MATCH("choices", ck.str, ck.len)) {
				if (v.len == 0)
					WT_RET_MSG(session, EINVAL,
					    "Key '%.*s' requires a value",
					    (int)k.len, k.str);
				if (v.type == WT_CONFIG_ITEM_STRUCT) {
					/*
					 * Handle the 'verbose' case of a list
					 * containing restricted choices.
					 */
					WT_RET(__wt_config_subinit(session,
					    &sparser, &v));
					found = 1;
					while (found &&
					    (ret = __wt_config_next(&sparser,
					    &v, &dummy)) == 0) {
						ret = __wt_config_subgetraw(
						    session, &cv, &v, &dummy);
						found = (ret == 0);
					}
				} else  {
					ret = __wt_config_subgetraw(session,
					    &cv, &v, &dummy);
					found = (ret == 0);
				}

				if (ret != 0 && ret != WT_NOTFOUND)
					return (ret);
				if (!found)
					WT_RET_MSG(session, EINVAL,
					    "Value '%.*s' not a "
					    "permitted choice for key '%.*s'",
					    (int)v.len, v.str,
					    (int)k.len, k.str);
			} else
				WT_RET_MSG(session, EINVAL,
				    "unexpected configuration description "
				    "keyword %.*s", (int)ck.len, ck.str);
		}
	}

	if (ret == WT_NOTFOUND)
		ret = 0;

	return (ret);
}
