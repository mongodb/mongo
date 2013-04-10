/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int config_check(
    WT_SESSION_IMPL *, const WT_CONFIG_CHECK *, const char *, size_t);

/*
 * configure_copy --
 *	Copy the default configuration in preparation for an extension.
 */
static int
configure_copy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*
	 * Unless the application extends the configuration information, we
	 * use a shared, read-only copy.  If the application wants to extend
	 * it, make a local copy and replace the original.  Our caller is
	 * holding the (probably unnecessary) lock to avoid a race.
	 */
	WT_RET(__wt_calloc_def(
	    session, conn->config_entries_count, &conn->config_entries_copy));
	memcpy(conn->config_entries_copy, conn->config_entries,
	    conn->config_entries_count * sizeof(WT_CONFIG_ENTRY));
	conn->config_entries = conn->config_entries_copy;
	return (0);
}

/*
 * __wt_conn_config_discard --
 *	Discard the connection's configuration table.
 */
void
__wt_conn_config_discard(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_CONFIG_ENTRY *ep, *f, *next;

	conn = S2C(session);

	/*
	 * If we never extended the table, we never allocated a copy, we're
	 * done.
	 */
	if (conn->config_entries_copy == NULL)
		return;

	/* Free any extensions, then free the copied table. */
	for (ep = conn->config_entries_copy; ep->method != NULL; ++ep)
		for (f = ep->extend; f != NULL; f = next) {
			next = f->extend;

			__wt_free(session, f->method);
			__wt_free(session, f->uri);
			__wt_free(session, f->name);
			__wt_free(session, f->checks->name);
			__wt_free(session, f->checks->type);
			__wt_free(session, f->checks->checks);
			__wt_free(session, f);
		}
	__wt_free(session, conn->config_entries_copy);
}

/*
 * __wt_configure_method --
 *	WT_CONNECTION.configure_method.
 */
int
__wt_configure_method(WT_SESSION_IMPL *session,
    const char *method, const char *uri,
    const char *name, const char *type, const char *check)
{
	WT_CONNECTION_IMPL *conn;
	WT_CONFIG_ENTRY *ep, *entry;
	WT_CONFIG_CHECK *checks;
	WT_DECL_RET;
	char *p, *t;

	conn = S2C(session);
	entry = NULL;
	checks = NULL;

	/*
	 * If we haven't yet allocated our local copy of the configuration
	 * information, do so now.
	 */
	if (conn->config_entries_copy == NULL) {
		__wt_spin_lock(session, &conn->api_lock);
		ret = configure_copy(session);
		__wt_spin_unlock(session, &conn->api_lock);
		WT_RET(ret);
	}

	/* Find a match for the method. */
	for (ep = conn->config_entries_copy; ep->method != NULL; ++ep)
		if (strcmp(ep->method, method) == 0)
			break;
	if (ep == NULL)
		WT_RET_MSG(session,
		    WT_NOTFOUND, "no method matching %s found", method);

	/* Allocate an extension entry. */
	WT_ERR(__wt_calloc_def(session, 1, &entry));
	if (uri != NULL)
		WT_ERR(__wt_strdup(session, uri, &entry->uri));
	if (name == NULL)
		WT_ERR_MSG(session, EINVAL, "no name specified");
	WT_ERR(__wt_strdup(session, name, &entry->name));
	WT_ERR(__wt_calloc_def(session, 2, &checks));
	/*
	 * There may be a default value in the name specified, we don't that as
	 * part of the checks name field.
	 */
	WT_ERR(__wt_strdup(session, name, &p));
	if ((t = strchr(p, '=')) != NULL)
		*t = '\0';
	checks->name = p;
	if (type == NULL)
		WT_ERR_MSG(session, EINVAL, "no type specified");
	WT_ERR(__wt_strdup(session, type, &checks->type));
	if (check != NULL)
		WT_ERR(__wt_strdup(session, check, &checks->checks));

	entry->checks = checks;

	/* Confirm the configuration string passes any checks we're given. */
	if (check != NULL)
		WT_ERR(config_check(session, checks, name, 0));

	/*
	 * Technically possible for threads to race, lock the connection while
	 * appending the new value to the method's linked list of configuration
	 * information.
	 */
	__wt_spin_lock(session, &conn->api_lock);
	while (ep->extend != NULL)
		ep = ep->extend;
	ep->extend = entry;
	__wt_spin_unlock(session, &conn->api_lock);

	return (0);

err:	if (entry != NULL) {
		__wt_free(session, entry->method);
		__wt_free(session, entry->uri);
		__wt_free(session, entry->name);
	}
	if (checks != NULL) {
		__wt_free(session, checks->name);
		__wt_free(session, checks->type);
		__wt_free(session, checks->checks);
	}
	return (ret);
}

/*
 * __wt_config_check--
 *	Check the keys in an application-supplied config string match what is
 * specified in all of the entry's check strings.
 */
int
__wt_config_check(WT_SESSION_IMPL *session,
    const WT_CONFIG_ENTRY *entry, const char *config, size_t config_len)
{
	const WT_CONFIG_CHECK *checks, *cp;
	const WT_CONFIG_ENTRY *ep;
	WT_CONFIG_CHECK *copy;
	WT_DECL_RET;
	size_t cnt;

	copy = NULL;

	/* It is always okay to not provide a configuration string. */
	if (config == NULL)
		return (0);

	/*
	 * If the call was ever extended build a combined set of checks.  This
	 * is slow, and it's per call to the configuration method, but I don't
	 * see much reason to get fancy until this gets used a lot more than I
	 * expect it will be.
	 *
	 * It's always okay to not provide any checks.
	 */
	if (entry->extend == NULL) {
		if ((checks = entry->checks) == NULL)
			return (0);
	} else {
		for (cnt = 0, ep = entry; ep != NULL; ep = ep->extend)
			for (cp = ep->checks; cp->name != NULL; ++cp)
				++cnt;
		if (cnt == 0)
			return (0);
		WT_RET(__wt_calloc_def(session, cnt + 1, &copy));
		for (cnt = 0, ep = entry; ep != NULL; ep = ep->extend)
			for (cp = ep->checks; cp->name != NULL; ++cp)
				copy[cnt++] = *cp;
		checks = copy;
	}

	ret = config_check(session, checks, config, config_len);

	if (copy != NULL)
		__wt_free(session, copy);
	return (ret);
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
		if (k.type != ITEM_STRING && k.type != ITEM_ID)
			WT_RET_MSG(session, EINVAL,
			    "Invalid configuration key found: '%.*s'",
			    (int)k.len, k.str);

		/* Search for a matching entry. */
		for (i = 0; checks[i].name != NULL; i++)
			if (WT_STRING_CASE_MATCH(checks[i].name, k.str, k.len))
				break;
		if (checks[i].name == NULL)
			WT_RET_MSG(session, EINVAL,
			    "unknown configuration key: '%.*s'",
			    (int)k.len, k.str);

		if (strcmp(checks[i].type, "boolean") == 0) {
			badtype = (v.type != ITEM_BOOL &&
			    (v.type != ITEM_NUM ||
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
			badtype = (v.type != ITEM_NUM);
		} else if (strcmp(checks[i].type, "list") == 0) {
			badtype = (v.len > 0 && v.type != ITEM_STRUCT);
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
				if (v.type == ITEM_STRUCT) {
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
