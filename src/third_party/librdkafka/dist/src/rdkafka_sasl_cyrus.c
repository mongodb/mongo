/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015-2022, Magnus Edenhill
 *               2023, Confluent Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rdkafka_int.h"
#include "rdkafka_transport.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_sasl.h"
#include "rdkafka_sasl_int.h"
#include "rdstring.h"

#if defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/wait.h> /* For WIF.. */
#endif

#ifdef __APPLE__
/* Apple has deprecated most of the SASL API for unknown reason,
 * silence those warnings. */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <sasl/sasl.h>

/**
 * @brief Process-global lock to avoid simultaneous invocation of
 *        kinit.cmd when refreshing the tickets, which could lead to
 *        kinit cache corruption.
 */
static mtx_t rd_kafka_sasl_cyrus_kinit_lock;

/**
 * @struct Per-client-instance handle
 */
typedef struct rd_kafka_sasl_cyrus_handle_s {
        rd_kafka_timer_t kinit_refresh_tmr;
        rd_atomic32_t ready; /**< First kinit command has finished, or there
                              *   is no kinit command. */
} rd_kafka_sasl_cyrus_handle_t;

/**
 * @struct Per-connection state
 */
typedef struct rd_kafka_sasl_cyrus_state_s {
        sasl_conn_t *conn;
        sasl_callback_t callbacks[16];
} rd_kafka_sasl_cyrus_state_t;



/**
 * Handle received frame from broker.
 */
static int rd_kafka_sasl_cyrus_recv(struct rd_kafka_transport_s *rktrans,
                                    const void *buf,
                                    size_t size,
                                    char *errstr,
                                    size_t errstr_size) {
        rd_kafka_sasl_cyrus_state_t *state = rktrans->rktrans_sasl.state;
        int r;
        int sendcnt = 0;

        if (rktrans->rktrans_sasl.complete && size == 0)
                goto auth_successful;

        do {
                sasl_interact_t *interact = NULL;
                const char *out;
                unsigned int outlen;

                mtx_lock(&rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.lock);
                r = sasl_client_step(state->conn, size > 0 ? buf : NULL, size,
                                     &interact, &out, &outlen);
                mtx_unlock(&rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.lock);

                if (r >= 0) {
                        /* Note: outlen may be 0 here for an empty response */
                        if (rd_kafka_sasl_send(rktrans, out, outlen, errstr,
                                               errstr_size) == -1)
                                return -1;
                        sendcnt++;
                }

                if (r == SASL_INTERACT)
                        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASL",
                                   "SASL_INTERACT: %lu %s, %s, %s, %p",
                                   interact->id, interact->challenge,
                                   interact->prompt, interact->defresult,
                                   interact->result);

        } while (r == SASL_INTERACT);

        if (r == SASL_CONTINUE)
                return 0; /* Wait for more data from broker */
        else if (r != SASL_OK) {
                rd_snprintf(errstr, errstr_size,
                            "SASL handshake failed (step): %s",
                            sasl_errdetail(state->conn));
                return -1;
        }

        if (!rktrans->rktrans_sasl.complete && sendcnt > 0) {
                /* With SaslAuthenticateRequest Kafka protocol framing
                 * we'll get a Response back after authentication is done,
                 * which should not be processed by Cyrus, but we still
                 * need to wait for the response to propgate its error,
                 * if any, before authentication is considered done.
                 *
                 * The legacy framing does not have a final broker->client
                 * response. */
                rktrans->rktrans_sasl.complete = 1;

                if (rktrans->rktrans_rkb->rkb_features &
                    RD_KAFKA_FEATURE_SASL_AUTH_REQ) {
                        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASL",
                                   "%s authentication complete but awaiting "
                                   "final response from broker",
                                   rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl
                                       .mechanisms);
                        return 0;
                }
        }

        /* Authentication successful */
auth_successful:
        if (rktrans->rktrans_rkb->rkb_rk->rk_conf.debug &
            RD_KAFKA_DBG_SECURITY) {
                const char *user, *mech, *authsrc;

                mtx_lock(&rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.lock);
                if (sasl_getprop(state->conn, SASL_USERNAME,
                                 (const void **)&user) != SASL_OK)
                        user = "(unknown)";
                mtx_unlock(&rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.lock);

                if (sasl_getprop(state->conn, SASL_MECHNAME,
                                 (const void **)&mech) != SASL_OK)
                        mech = "(unknown)";

                if (sasl_getprop(state->conn, SASL_AUTHSOURCE,
                                 (const void **)&authsrc) != SASL_OK)
                        authsrc = "(unknown)";

                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASL",
                           "Authenticated as %s using %s (%s)", user, mech,
                           authsrc);
        }

        rd_kafka_sasl_auth_done(rktrans);

        return 0;
}



static ssize_t
render_callback(const char *key, char *buf, size_t size, void *opaque) {
        rd_kafka_t *rk = opaque;
        rd_kafka_conf_res_t res;
        size_t destsize = size;

        /* Try config lookup. */
        res = rd_kafka_conf_get(&rk->rk_conf, key, buf, &destsize);
        if (res != RD_KAFKA_CONF_OK)
                return -1;

        /* Dont include \0 in returned size */
        return (destsize > 0 ? destsize - 1 : destsize);
}


/**
 * @brief Execute kinit to refresh ticket.
 *
 * @returns 0 on success, -1 on error.
 *
 * @locality rdkafka main thread
 */
static int rd_kafka_sasl_cyrus_kinit_refresh(rd_kafka_t *rk) {
        rd_kafka_sasl_cyrus_handle_t *handle = rk->rk_sasl.handle;
        int r;
        char *cmd;
        char errstr[128];
        rd_ts_t ts_start;
        int duration;

        /* Build kinit refresh command line using string rendering and config */
        cmd = rd_string_render(rk->rk_conf.sasl.kinit_cmd, errstr,
                               sizeof(errstr), render_callback, rk);
        if (!cmd) {
                rd_kafka_log(rk, LOG_ERR, "SASLREFRESH",
                             "Failed to construct kinit command "
                             "from sasl.kerberos.kinit.cmd template: %s",
                             errstr);
                return -1;
        }

        /* Execute kinit */
        rd_kafka_dbg(rk, SECURITY, "SASLREFRESH",
                     "Refreshing Kerberos ticket with command: %s", cmd);

        ts_start = rd_clock();

        /* Prevent multiple simultaneous refreshes by the same process to
         * avoid Kerberos credential cache corruption. */
        mtx_lock(&rd_kafka_sasl_cyrus_kinit_lock);
        r = system(cmd);
        mtx_unlock(&rd_kafka_sasl_cyrus_kinit_lock);

        duration = (int)((rd_clock() - ts_start) / 1000);
        if (duration > 5000)
                rd_kafka_log(rk, LOG_WARNING, "SASLREFRESH",
                             "Slow Kerberos ticket refresh: %dms: %s", duration,
                             cmd);

        /* Regardless of outcome from the kinit command (it can fail
         * even if the ticket is available), we now allow broker connections. */
        if (rd_atomic32_add(&handle->ready, 1) == 1) {
                rd_kafka_dbg(rk, SECURITY, "SASLREFRESH",
                             "First kinit command finished: waking up "
                             "broker threads");
                rd_kafka_all_brokers_wakeup(rk, RD_KAFKA_BROKER_STATE_INIT,
                                            "Kerberos ticket refresh");
        }

        if (r == -1) {
                if (errno == ECHILD) {
                        rd_kafka_log(rk, LOG_WARNING, "SASLREFRESH",
                                     "Kerberos ticket refresh command "
                                     "returned ECHILD: %s: exit status "
                                     "unknown, assuming success",
                                     cmd);
                } else {
                        rd_kafka_log(rk, LOG_ERR, "SASLREFRESH",
                                     "Kerberos ticket refresh failed: %s: %s",
                                     cmd, rd_strerror(errno));
                        rd_free(cmd);
                        return -1;
                }
        } else if (WIFSIGNALED(r)) {
                rd_kafka_log(rk, LOG_ERR, "SASLREFRESH",
                             "Kerberos ticket refresh failed: %s: "
                             "received signal %d",
                             cmd, WTERMSIG(r));
                rd_free(cmd);
                return -1;
        } else if (WIFEXITED(r) && WEXITSTATUS(r) != 0) {
                rd_kafka_log(rk, LOG_ERR, "SASLREFRESH",
                             "Kerberos ticket refresh failed: %s: "
                             "exited with code %d",
                             cmd, WEXITSTATUS(r));
                rd_free(cmd);
                return -1;
        }

        rd_free(cmd);

        rd_kafka_dbg(rk, SECURITY, "SASLREFRESH",
                     "Kerberos ticket refreshed in %dms", duration);
        return 0;
}


/**
 * @brief Refresh timer callback
 *
 * @locality rdkafka main thread
 */
static void rd_kafka_sasl_cyrus_kinit_refresh_tmr_cb(rd_kafka_timers_t *rkts,
                                                     void *arg) {
        rd_kafka_t *rk = arg;

        rd_kafka_sasl_cyrus_kinit_refresh(rk);
}



/**
 *
 * libsasl callbacks
 *
 */
static RD_UNUSED int rd_kafka_sasl_cyrus_cb_getopt(void *context,
                                                   const char *plugin_name,
                                                   const char *option,
                                                   const char **result,
                                                   unsigned *len) {
        rd_kafka_transport_t *rktrans = context;

        if (!strcmp(option, "client_mech_list"))
                *result = "GSSAPI";
        if (!strcmp(option, "canon_user_plugin"))
                *result = "INTERNAL";

        if (*result && len)
                *len = strlen(*result);

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "LIBSASL",
                   "CB_GETOPT: plugin %s, option %s: returning %s", plugin_name,
                   option, *result);

        return SASL_OK;
}

static int
rd_kafka_sasl_cyrus_cb_log(void *context, int level, const char *message) {
        rd_kafka_transport_t *rktrans = context;

        /* Provide a more helpful error message in case Kerberos
         * plugins are missing. */
        if (strstr(message, "No worthy mechs found") &&
            strstr(rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.mechanisms,
                   "GSSAPI"))
                message =
                    "Cyrus/libsasl2 is missing a GSSAPI module: "
                    "make sure the libsasl2-modules-gssapi-mit or "
                    "cyrus-sasl-gssapi packages are installed";

        /* Treat the "client step" log messages as debug. */
        if (level >= LOG_DEBUG || !strncmp(message, "GSSAPI client step ", 19))
                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "LIBSASL", "%s",
                           message);
        else
                rd_rkb_log(rktrans->rktrans_rkb, level, "LIBSASL", "%s",
                           message);

        return SASL_OK;
}


static int rd_kafka_sasl_cyrus_cb_getsimple(void *context,
                                            int id,
                                            const char **result,
                                            unsigned *len) {
        rd_kafka_transport_t *rktrans = context;

        switch (id) {
        case SASL_CB_USER:
        case SASL_CB_AUTHNAME:
                /* Since cyrus expects the returned pointer to be stable
                 * and not have its content changed, but the username
                 * and password may be updated at anytime by the application
                 * calling sasl_set_credentials(), we need to lock
                 * rk_conf.sasl.lock before each call into cyrus-sasl.
                 * So when we get here the lock is already held. */
                *result = rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.username;
                break;

        default:
                *result = NULL;
                break;
        }

        if (len)
                *len = *result ? strlen(*result) : 0;

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "LIBSASL",
                   "CB_GETSIMPLE: id 0x%x: returning %s", id, *result);

        return *result ? SASL_OK : SASL_FAIL;
}


static int rd_kafka_sasl_cyrus_cb_getsecret(sasl_conn_t *conn,
                                            void *context,
                                            int id,
                                            sasl_secret_t **psecret) {
        rd_kafka_transport_t *rktrans = context;
        const char *password;

        /* rk_conf.sasl.lock is already locked */
        password = rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.password;

        if (!password) {
                *psecret = NULL;
        } else {
                size_t passlen = strlen(password);
                *psecret = rd_realloc(*psecret, sizeof(**psecret) + passlen);
                (*psecret)->len = passlen;
                memcpy((*psecret)->data, password, passlen);
        }

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "LIBSASL",
                   "CB_GETSECRET: id 0x%x: returning %s", id,
                   *psecret ? "(hidden)" : "NULL");

        return SASL_OK;
}

static int rd_kafka_sasl_cyrus_cb_chalprompt(void *context,
                                             int id,
                                             const char *challenge,
                                             const char *prompt,
                                             const char *defres,
                                             const char **result,
                                             unsigned *len) {
        rd_kafka_transport_t *rktrans = context;

        *result = "min_chalprompt";
        *len    = strlen(*result);

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "LIBSASL",
                   "CB_CHALPROMPT: id 0x%x, challenge %s, prompt %s, "
                   "default %s: returning %s",
                   id, challenge, prompt, defres, *result);

        return SASL_OK;
}

static int rd_kafka_sasl_cyrus_cb_getrealm(void *context,
                                           int id,
                                           const char **availrealms,
                                           const char **result) {
        rd_kafka_transport_t *rktrans = context;

        *result = *availrealms;

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "LIBSASL",
                   "CB_GETREALM: id 0x%x: returning %s", id, *result);

        return SASL_OK;
}


static RD_UNUSED int rd_kafka_sasl_cyrus_cb_canon(sasl_conn_t *conn,
                                                  void *context,
                                                  const char *in,
                                                  unsigned inlen,
                                                  unsigned flags,
                                                  const char *user_realm,
                                                  char *out,
                                                  unsigned out_max,
                                                  unsigned *out_len) {
        rd_kafka_transport_t *rktrans = context;

        if (strstr(rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.mechanisms,
                   "GSSAPI")) {
                *out_len = rd_snprintf(
                    out, out_max, "%s",
                    rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.principal);
        } else if (!strcmp(
                       rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.mechanisms,
                       "PLAIN")) {
                *out_len = rd_snprintf(out, out_max, "%.*s", inlen, in);
        } else
                out = NULL;

        rd_rkb_dbg(
            rktrans->rktrans_rkb, SECURITY, "LIBSASL",
            "CB_CANON: flags 0x%x, \"%.*s\" @ \"%s\": returning \"%.*s\"",
            flags, (int)inlen, in, user_realm, (int)(*out_len), out);

        return out ? SASL_OK : SASL_FAIL;
}


static void rd_kafka_sasl_cyrus_close(struct rd_kafka_transport_s *rktrans) {
        rd_kafka_sasl_cyrus_state_t *state = rktrans->rktrans_sasl.state;

        if (!state)
                return;

        if (state->conn) {
                mtx_lock(&rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.lock);
                sasl_dispose(&state->conn);
                mtx_unlock(&rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.lock);
        }
        rd_free(state);
        rktrans->rktrans_sasl.state = NULL;
}


/**
 * Initialize and start SASL authentication.
 *
 * Returns 0 on successful init and -1 on error.
 *
 * Locality: broker thread
 */
static int rd_kafka_sasl_cyrus_client_new(rd_kafka_transport_t *rktrans,
                                          const char *hostname,
                                          char *errstr,
                                          size_t errstr_size) {
        int r;
        rd_kafka_sasl_cyrus_state_t *state;
        rd_kafka_broker_t *rkb        = rktrans->rktrans_rkb;
        rd_kafka_t *rk                = rkb->rkb_rk;
        sasl_callback_t callbacks[16] = {
            // { SASL_CB_GETOPT, (void *)rd_kafka_sasl_cyrus_cb_getopt, rktrans
            // },
            {SASL_CB_LOG, (void *)rd_kafka_sasl_cyrus_cb_log, rktrans},
            {SASL_CB_AUTHNAME, (void *)rd_kafka_sasl_cyrus_cb_getsimple,
             rktrans},
            {SASL_CB_PASS, (void *)rd_kafka_sasl_cyrus_cb_getsecret, rktrans},
            {SASL_CB_ECHOPROMPT, (void *)rd_kafka_sasl_cyrus_cb_chalprompt,
             rktrans},
            {SASL_CB_GETREALM, (void *)rd_kafka_sasl_cyrus_cb_getrealm,
             rktrans},
            {SASL_CB_CANON_USER, (void *)rd_kafka_sasl_cyrus_cb_canon, rktrans},
            {SASL_CB_LIST_END}};

        state                       = rd_calloc(1, sizeof(*state));
        rktrans->rktrans_sasl.state = state;

        /* SASL_CB_USER is needed for PLAIN but breaks GSSAPI */
        if (!strcmp(rk->rk_conf.sasl.mechanisms, "PLAIN")) {
                int endidx;
                /* Find end of callbacks array */
                for (endidx = 0; callbacks[endidx].id != SASL_CB_LIST_END;
                     endidx++)
                        ;

                callbacks[endidx].id = SASL_CB_USER;
                callbacks[endidx].proc =
                    (void *)rd_kafka_sasl_cyrus_cb_getsimple;
                callbacks[endidx].context = rktrans;
                endidx++;
                callbacks[endidx].id = SASL_CB_LIST_END;
        }

        memcpy(state->callbacks, callbacks, sizeof(callbacks));

        mtx_lock(&rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.lock);
        r = sasl_client_new(rk->rk_conf.sasl.service_name, hostname, NULL,
                            NULL, /* no local & remote IP checks */
                            state->callbacks, 0, &state->conn);
        mtx_unlock(&rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.lock);
        if (r != SASL_OK) {
                rd_snprintf(errstr, errstr_size, "%s",
                            sasl_errstring(r, NULL, NULL));
                return -1;
        }

        if (rk->rk_conf.debug & RD_KAFKA_DBG_SECURITY) {
                const char *avail_mechs;
                sasl_listmech(state->conn, NULL, NULL, " ", NULL, &avail_mechs,
                              NULL, NULL);
                rd_rkb_dbg(rkb, SECURITY, "SASL",
                           "My supported SASL mechanisms: %s", avail_mechs);
        }

        do {
                const char *out;
                unsigned int outlen;
                const char *mech = NULL;

                mtx_lock(&rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.lock);
                r = sasl_client_start(state->conn, rk->rk_conf.sasl.mechanisms,
                                      NULL, &out, &outlen, &mech);
                mtx_unlock(&rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.lock);

                if (r >= 0)
                        if (rd_kafka_sasl_send(rktrans, out, outlen, errstr,
                                               errstr_size))
                                return -1;
        } while (r == SASL_INTERACT);

        if (r == SASL_OK) {
                /* PLAIN is appearantly done here, but we still need to make
                 * sure the PLAIN frame is sent and we get a response back (but
                 * we must not pass the response to libsasl or it will fail). */
                rktrans->rktrans_sasl.complete = 1;
                return 0;

        } else if (r != SASL_CONTINUE) {
                rd_snprintf(errstr, errstr_size,
                            "SASL handshake failed (start (%d)): %s", r,
                            sasl_errdetail(state->conn));
                return -1;
        }

        return 0;
}


/**
 * @brief SASL/GSSAPI is ready when at least one kinit command has been
 *        executed (regardless of exit status).
 */
static rd_bool_t rd_kafka_sasl_cyrus_ready(rd_kafka_t *rk) {
        rd_kafka_sasl_cyrus_handle_t *handle = rk->rk_sasl.handle;
        if (!rk->rk_conf.sasl.relogin_min_time)
                return rd_true;
        if (!handle)
                return rd_false;

        return rd_atomic32_get(&handle->ready) > 0;
}

/**
 * @brief Per-client-instance initializer
 */
static int
rd_kafka_sasl_cyrus_init(rd_kafka_t *rk, char *errstr, size_t errstr_size) {
        rd_kafka_sasl_cyrus_handle_t *handle;

        if (!rk->rk_conf.sasl.relogin_min_time || !rk->rk_conf.sasl.kinit_cmd ||
            strcmp(rk->rk_conf.sasl.mechanisms, "GSSAPI"))
                return 0; /* kinit not configured, no need to start timer */

        handle             = rd_calloc(1, sizeof(*handle));
        rk->rk_sasl.handle = handle;

        rd_kafka_timer_start(&rk->rk_timers, &handle->kinit_refresh_tmr,
                             rk->rk_conf.sasl.relogin_min_time * 1000ll,
                             rd_kafka_sasl_cyrus_kinit_refresh_tmr_cb, rk);

        /* Kick off the timer immediately to refresh the ticket.
         * (Timer is triggered from the main loop). */
        rd_kafka_timer_override_once(&rk->rk_timers, &handle->kinit_refresh_tmr,
                                     0 /*immediately*/);

        return 0;
}


/**
 * @brief Per-client-instance destructor
 */
static void rd_kafka_sasl_cyrus_term(rd_kafka_t *rk) {
        rd_kafka_sasl_cyrus_handle_t *handle = rk->rk_sasl.handle;

        if (!handle)
                return;

        rd_kafka_timer_stop(&rk->rk_timers, &handle->kinit_refresh_tmr, 1);
        rd_free(handle);
        rk->rk_sasl.handle = NULL;
}


static int rd_kafka_sasl_cyrus_conf_validate(rd_kafka_t *rk,
                                             char *errstr,
                                             size_t errstr_size) {

        if (strcmp(rk->rk_conf.sasl.mechanisms, "GSSAPI"))
                return 0;

        if (rk->rk_conf.sasl.relogin_min_time && rk->rk_conf.sasl.kinit_cmd) {
                char *cmd;
                char tmperr[128];

                cmd = rd_string_render(rk->rk_conf.sasl.kinit_cmd, tmperr,
                                       sizeof(tmperr), render_callback, rk);

                if (!cmd) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid sasl.kerberos.kinit.cmd value: %s",
                                    tmperr);
                        return -1;
                }

                rd_free(cmd);
        }

        return 0;
}


/**
 * Global SASL termination.
 */
void rd_kafka_sasl_cyrus_global_term(void) {
        /* NOTE: Should not be called since the application may be using SASL
         * too*/
        /* sasl_done(); */
        mtx_destroy(&rd_kafka_sasl_cyrus_kinit_lock);
}


/**
 * Global SASL init, called once per runtime.
 */
int rd_kafka_sasl_cyrus_global_init(void) {
        int r;

        mtx_init(&rd_kafka_sasl_cyrus_kinit_lock, mtx_plain);

        r = sasl_client_init(NULL);
        if (r != SASL_OK) {
                fprintf(stderr, "librdkafka: sasl_client_init() failed: %s\n",
                        sasl_errstring(r, NULL, NULL));
                return -1;
        }

        return 0;
}


const struct rd_kafka_sasl_provider rd_kafka_sasl_cyrus_provider = {
    .name          = "Cyrus",
    .init          = rd_kafka_sasl_cyrus_init,
    .term          = rd_kafka_sasl_cyrus_term,
    .client_new    = rd_kafka_sasl_cyrus_client_new,
    .recv          = rd_kafka_sasl_cyrus_recv,
    .close         = rd_kafka_sasl_cyrus_close,
    .ready         = rd_kafka_sasl_cyrus_ready,
    .conf_validate = rd_kafka_sasl_cyrus_conf_validate};
