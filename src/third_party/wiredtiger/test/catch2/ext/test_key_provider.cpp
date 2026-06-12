/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch.hpp>

#include "ext/test/key_provider/key_provider.h"

#include "wrappers/connection_wrapper.h"
#include "utils.h"

#include "wiredtiger.h"
#include "wt_internal.h"

/*
 * kp_fixture
 *     Test fixture for the key provider extension tests.
 */
struct kp_fixture {
    using extension_init_t = decltype(&wiredtiger_extension_init);
    extension_init_t extension_init = nullptr;

    connection_wrapper conn;
    WT_SESSION *session = nullptr;

    KEY_PROVIDER *kp = nullptr;
    using kp_ptr_t = std::unique_ptr<KEY_PROVIDER, std::function<void(KEY_PROVIDER *)>>;

    static extension_init_t
    get_init_proc()
    {
#if defined(HAVE_BUILTIN_EXTENSION_KEY_PROVIDER)
        return key_provider_extension_init;
#else
        static utils::shared_library lib{KEY_PROVIDER_EXTENSION};
        return lib.get<extension_init_t>("wiredtiger_extension_init");
#endif /* HAVE_BUILTIN_EXTENSION_KEY_PROVIDER */
    }

    ~kp_fixture()
    {
        kp_reset();
        session->close(session, nullptr);
        session = nullptr;
    }

    kp_fixture() : extension_init(get_init_proc()), conn(DB_HOME, "create,in_memory")
    {
        REQUIRE(conn.get_wt_connection()->open_session(
                  conn.get_wt_connection(), NULL, NULL, &session) == 0);
        REQUIRE(session != nullptr);
    }

    kp_ptr_t
    kp_init(const char *config)
    {
        const char *ext_config[] = {config, nullptr};

        int ret =
          extension_init(conn.get_wt_connection(), reinterpret_cast<WT_CONFIG_ARG *>(ext_config));
        REQUIRE(ret == 0);

        REQUIRE(kp == nullptr);
        kp = reinterpret_cast<KEY_PROVIDER *>(conn.get_wt_connection_impl()->key_provider);
        REQUIRE(kp != nullptr);

        return kp_ptr_t(kp, [this](KEY_PROVIDER *k) {
            assert(k == this->kp);
            this->kp_reset();
        });
    }

    void
    kp_reset()
    {
        if (kp != nullptr) {
            int ret = kp->iface.terminate(&kp->iface, session);
            if (ret != 0) {
                WARN("Error terminating key provider: " << ret);
            }
            kp = nullptr;
        }
        conn.get_wt_connection_impl()->key_provider = nullptr;
    }

    void
    kp_load_key(const std::string &key_data, uint64_t lsn)
    {
        REQUIRE(kp != nullptr);
        WT_KEY_PROVIDER *wtkp = &kp->iface;

        WT_CRYPT_KEYS crypt = {};
        crypt.r.lsn = lsn;
        crypt.keys.data = key_data.data();
        crypt.keys.size = key_data.size();

        REQUIRE(wtkp->load_key(wtkp, session, &crypt) == 0);

        REQUIRE(kp->state.key_state == KEY_STATE_CURRENT);
        REQUIRE(kp->state.lsn == lsn);
        REQUIRE(kp->state.key_time != 0);
        REQUIRE(kp->state.key_size == key_data.size());
        REQUIRE(memcmp(kp->state.key_data, key_data.data(), key_data.size()) == 0);
    }

    WT_CRYPT_KEYS
    kp_get_key()
    {
        REQUIRE(kp != nullptr);
        WT_KEY_PROVIDER *wtkp = &kp->iface;

        REQUIRE(kp->state.key_state == KEY_STATE_CURRENT);

        /* Get key; first query the size, then get the data */
        WT_CRYPT_KEYS crypt = {};
        REQUIRE(wtkp->get_key(wtkp, session, &crypt) == 0);

        REQUIRE(kp->state.key_state == KEY_STATE_PENDING);
        REQUIRE(kp->state.lsn == 0);   /* New key is not queued yet */
        REQUIRE(crypt.keys.size != 0); /* Key has changed */
        REQUIRE(crypt.keys.data == nullptr);

        crypt.keys.data = malloc(crypt.keys.size);
        REQUIRE(wtkp->get_key(wtkp, session, &crypt) == 0);

        REQUIRE(kp->state.key_state == KEY_STATE_READ);
        REQUIRE(kp->state.lsn == 0); /* New key is not queued yet */
        REQUIRE(crypt.keys.size == kp->state.key_size);
        REQUIRE(memcmp(crypt.keys.data, kp->state.key_data, crypt.keys.size) == 0);

        return (crypt);
    }

    int
    push_key(WT_KEY_PROVIDER *provider, const std::string &key_bytes, uint64_t timestamp)
    {
        WT_CRYPT_KEYS crypt = {};
        crypt.keys.data = key_bytes.data();
        crypt.keys.size = key_bytes.size();
        crypt.timestamp = timestamp;
        return (provider->set_key(provider, session, &crypt));
    }
};

/*
 * validate_chosen_key --
 *     Assert the entry a checkpoint selected carries the expected timestamp and key bytes.
 */
static void
validate_chosen_key(
  WT_DISAGG_PENDING_CRYPT_KEY *entry, uint64_t timestamp, const std::string &key_bytes)
{
    REQUIRE(entry != nullptr);
    REQUIRE(entry->timestamp == timestamp);
    REQUIRE(entry->keys.size == key_bytes.size());
    REQUIRE(memcmp(entry->keys.data, key_bytes.data(), key_bytes.size()) == 0);
}

/*
 * validate_pending_queue --
 *     Assert the pending queue holds exactly the expected (timestamp, key bytes) entries in order.
 *     Used to confirm a checkpoint pruned the queue correctly.
 */
static void
validate_pending_queue(
  WT_CONNECTION_IMPL *conn_impl, const std::vector<std::pair<uint64_t, std::string>> &expected)
{
    WT_DISAGG_PENDING_CRYPT_KEY *entry =
      TAILQ_FIRST(&conn_impl->disaggregated_storage.pending_crypt_key_qh);
    for (const auto &expect : expected) {
        validate_chosen_key(entry, expect.first, expect.second);
        entry = TAILQ_NEXT(entry, q);
    }
    REQUIRE(entry == nullptr); /* No unexpected trailing entries. */
}

TEST_CASE_METHOD(kp_fixture, "Config", "[key_provider]")
{
    SECTION("Null config")
    {
        kp_ptr_t kp = kp_init(nullptr);
        REQUIRE(kp->wtext != nullptr);

        REQUIRE(kp->verbose == WT_VERBOSE_INFO);
        /* Default: 12 hours; negative indicates never used */
        REQUIRE(kp->key_expires == -43200);
    }

    SECTION("Empty config")
    {
        kp_ptr_t kp = kp_init("");
        REQUIRE(kp->wtext != nullptr);

        REQUIRE(kp->verbose == WT_VERBOSE_INFO);
        /* Default: 12 hours; negative indicates never used */
        REQUIRE(kp->key_expires == -43200);
    }

    SECTION("Custom config, regular key expiration")
    {
        kp_ptr_t kp = kp_init("verbose=2,key_expires=300");
        REQUIRE(kp->wtext != nullptr);

        REQUIRE(kp->verbose == WT_VERBOSE_DEBUG_2);
        /* Regular key expiration; negative indicates never used */
        REQUIRE(kp->key_expires == -300);
    }

    SECTION("Custom config, key always expires")
    {
        kp_ptr_t kp = kp_init("verbose=2,key_expires=0");
        REQUIRE(kp->wtext != nullptr);

        REQUIRE(kp->verbose == WT_VERBOSE_DEBUG_2);
        REQUIRE(kp->key_expires == 0);
    }

    SECTION("Invalid config")
    {
        const char *invalid_configs[] = {
          "verbose=invalid",
          "key_expires=invalid",
          "verb=0,key=1",
          "wrong=bad",
        };

        for (const char *config : invalid_configs) {
            const char *ext_config[] = {config, nullptr};
            int ret = extension_init(
              conn.get_wt_connection(), reinterpret_cast<WT_CONFIG_ARG *>(ext_config));
            REQUIRE(ret == EINVAL);
        }
    }
}

TEST_CASE_METHOD(kp_fixture, "Default values", "[key_provider]")
{
    kp_ptr_t kp = kp_init(nullptr);
    REQUIRE(kp->wtext != nullptr);

    /* Initial state */
    REQUIRE(kp->verbose == WT_VERBOSE_INFO);
    /* Default: 12 hours; negative indicates never used */
    REQUIRE(kp->key_expires == -43200);
    REQUIRE(kp->state.key_state == KEY_STATE_CURRENT);
    REQUIRE(kp->state.lsn == 0);
    REQUIRE(kp->state.key_time != 0);
    REQUIRE(kp->state.key_size != 0);
    REQUIRE(kp->state.key_data != nullptr);
}

TEST_CASE_METHOD(kp_fixture, "Load key", "[key_provider]")
{
    kp_ptr_t kp = kp_init(nullptr);
    REQUIRE(kp->wtext != nullptr);

    /* Key data must match DEFAULT_KEY_DATA */
    const std::string dummy_key = "abcdefghijklmnopqrstuvwxyz";
    const uint64_t dummy_lsn = 42;

    kp_load_key(dummy_key, dummy_lsn);
}

TEST_CASE_METHOD(kp_fixture, "Key one-shot expired", "[key_provider]")
{
    kp_ptr_t kp = kp_init(nullptr);
    REQUIRE(kp->wtext != nullptr);

    REQUIRE(kp->verbose == WT_VERBOSE_INFO);
    /* Default: 12 hours; negative indicates never used */
    REQUIRE(kp->key_expires == -43200);
    REQUIRE(kp->state.key_state == KEY_STATE_CURRENT);

    WT_KEY_PROVIDER *wtkp = &kp->iface;

    WT_CRYPT_KEYS crypt = {};
    crypt.keys.data = nullptr; /* Indicate request for key size */
    REQUIRE(wtkp->get_key(wtkp, session, &crypt) == 0);

    REQUIRE(kp->state.key_state == KEY_STATE_PENDING);
    REQUIRE(crypt.keys.size != 0); /* Key is pending for reading */
    REQUIRE(crypt.keys.data == nullptr);

    /* Advance key state as if read-update is done. */
    kp->state.key_state = KEY_STATE_CURRENT;

    /* Key is one-shot expired; next get_key should indicate no expiration */
    REQUIRE(wtkp->get_key(wtkp, session, &crypt) == 0);
    REQUIRE(crypt.keys.size == 0); /* Key has not changed */
}

TEST_CASE_METHOD(kp_fixture, "Key expired and rotated", "[key_provider]")
{
    kp_ptr_t kp = kp_init(nullptr);
    REQUIRE(kp->wtext != nullptr);

    REQUIRE(kp->verbose == WT_VERBOSE_INFO);
    /* Default: 12 hours; negative indicates never used */
    REQUIRE(kp->key_expires == -43200);
    REQUIRE(kp->state.key_state == KEY_STATE_CURRENT);

    const auto init_key_time = kp->state.key_time;

    /* Expire the key by setting the key_time to the past */
    kp->state.key_time -= (kp->key_expires + 1) * CLOCKS_PER_SEC;

    /* Generates a new key */
    WT_CRYPT_KEYS crypt = kp_get_key();
    const auto new_key_time = kp->state.key_time;
    REQUIRE(init_key_time != new_key_time); /* New key generated */

    free(const_cast<void *>(crypt.keys.data));
}

TEST_CASE_METHOD(kp_fixture, "Read key, success", "[key_provider]")
{
    kp_ptr_t kp = kp_init(nullptr);
    REQUIRE(kp->wtext != nullptr);

    /* Expire the key by setting the key_time to the past */
    kp->state.key_time -= (kp->key_expires + 1) * CLOCKS_PER_SEC;

    /* Generates a new key */
    WT_CRYPT_KEYS crypt = kp_get_key();

    WT_KEY_PROVIDER *wtkp = &kp->iface;

    const uint64_t new_lsn = 84; /* New LSN after queueing */
    crypt.r.lsn = new_lsn;
    REQUIRE(wtkp->on_key_update(wtkp, session, &crypt) == 0);
    /* LSN should be updated on success */
    REQUIRE(kp->state.lsn == new_lsn);
    REQUIRE(kp->state.key_state == KEY_STATE_CURRENT);

    free(const_cast<void *>(crypt.keys.data));
}

TEST_CASE_METHOD(kp_fixture, "Persist key, failure", "[key_provider]")
{
    kp_ptr_t kp = kp_init(nullptr);
    REQUIRE(kp->wtext != nullptr);

    /* Expire the key by setting the key_time to the past */
    kp->state.key_time -= (kp->key_expires + 1) * CLOCKS_PER_SEC;

    /* Generates a new key */
    WT_CRYPT_KEYS crypt = kp_get_key();

    WT_KEY_PROVIDER *wtkp = &kp->iface;

    /* Simulate key queueing failure */
    crypt.keys.size = 0; /* Indicate failure */
    crypt.r.error = EIO; /* I/O error */
    REQUIRE(wtkp->on_key_update(wtkp, session, &crypt) == 0);

    /* LSN should not be updated on failure */
    REQUIRE(kp->state.lsn == 0);
    REQUIRE(kp->state.key_state == KEY_STATE_READ);

    free(const_cast<void *>(crypt.keys.data));
}

TEST_CASE_METHOD(kp_fixture, "set_key_provider version selects push mode", "[key_provider]")
{
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_KEY_PROVIDER stub = {};

    /* version=0 (default): push flag stays clear. */
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=0") == 0);
    REQUIRE(!F_ISSET(conn_impl, WT_CONN_KEY_PROVIDER_PUSH));
    REQUIRE(stub.set_key == nullptr);  /* No WT-supplied set_key in pull mode. */
    conn_impl->key_provider = nullptr; /* Allow reconfiguration. */

    /* version=1: push flag is set. */
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=1") == 0);
    REQUIRE(F_ISSET(conn_impl, WT_CONN_KEY_PROVIDER_PUSH));
    REQUIRE(stub.set_key != nullptr); /* WiredTiger installs its set_key. */

    /* Cleanup so the fixture destructor doesn't see a stale provider. */
    conn_impl->key_provider = nullptr;
    F_CLR(conn_impl, WT_CONN_KEY_PROVIDER_PUSH);
}

TEST_CASE_METHOD(kp_fixture, "set_key stores the pushed key as the active key", "[key_provider]")
{
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_KEY_PROVIDER stub = {};
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=1") == 0);

    REQUIRE(TAILQ_EMPTY(&conn_impl->disaggregated_storage.pending_crypt_key_qh));

    const std::string key_bytes = "push-mode-test-key-0123456789";
    REQUIRE(push_key(&stub, key_bytes, 1) == 0);
    validate_pending_queue(conn_impl, {{1, key_bytes}});

    conn_impl->key_provider = nullptr;
    F_CLR(conn_impl, WT_CONN_KEY_PROVIDER_PUSH);
}

TEST_CASE_METHOD(kp_fixture, "set_key accumulates monotonic entries", "[key_provider]")
{
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_KEY_PROVIDER stub = {};
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=1") == 0);

    const std::string keys[3] = {"key-one-0123456789", "key-two-9876543210", "key-three-55555"};
    const uint64_t timestamps[3] = {10, 20, 30};
    for (int i = 0; i < 3; i++)
        REQUIRE(push_key(&stub, keys[i], timestamps[i]) == 0);

    validate_pending_queue(conn_impl, {{10, keys[0]}, {20, keys[1]}, {30, keys[2]}});

    conn_impl->key_provider = nullptr;
    F_CLR(conn_impl, WT_CONN_KEY_PROVIDER_PUSH);
}

TEST_CASE_METHOD(kp_fixture, "set_key rejects non-monotonic timestamp", "[key_provider]")
{
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_KEY_PROVIDER stub = {};
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=1") == 0);

    const std::string key_bytes = "push-mode-key-monotonic";
    REQUIRE(push_key(&stub, key_bytes, 10) == 0);
    REQUIRE(push_key(&stub, key_bytes, 10) == EINVAL); /* Equal rejected. */
    REQUIRE(push_key(&stub, key_bytes, 5) == EINVAL);  /* Smaller rejected. */
    REQUIRE(push_key(&stub, key_bytes, 11) == 0);      /* Larger accepted. */

    /* Only the two successful pushes are queued. */
    int n = 0;
    WT_DISAGG_PENDING_CRYPT_KEY *entry;
    TAILQ_FOREACH (entry, &conn_impl->disaggregated_storage.pending_crypt_key_qh, q)
        n++;
    REQUIRE(n == 2);

    conn_impl->key_provider = nullptr;
    F_CLR(conn_impl, WT_CONN_KEY_PROVIDER_PUSH);
}

TEST_CASE_METHOD(kp_fixture, "set_key rejects timestamp <= stable", "[key_provider]")
{
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_KEY_PROVIDER stub = {};
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=1") == 0);

    REQUIRE(wt_conn->set_timestamp(wt_conn, "stable_timestamp=14") == 0); /* 0x14 == 20. */

    const std::string key_bytes = "push-mode-stable-check";
    REQUIRE(push_key(&stub, key_bytes, 20) == EINVAL); /* Equal to stable. */
    REQUIRE(push_key(&stub, key_bytes, 10) == EINVAL); /* Below stable. */
    REQUIRE(push_key(&stub, key_bytes, 30) == 0);      /* Strictly above stable. */

    conn_impl->key_provider = nullptr;
    F_CLR(conn_impl, WT_CONN_KEY_PROVIDER_PUSH);
}

TEST_CASE_METHOD(kp_fixture, "set_key rejects empty input", "[key_provider]")
{
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_KEY_PROVIDER stub = {};
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=1") == 0);

    WT_CRYPT_KEYS crypt = {};
    crypt.keys.data = nullptr;
    crypt.keys.size = 0;
    crypt.timestamp = 1;
    REQUIRE(stub.set_key(&stub, session, &crypt) == EINVAL);

    conn_impl->key_provider = nullptr;
    F_CLR(conn_impl, WT_CONN_KEY_PROVIDER_PUSH);
}

TEST_CASE_METHOD(
  kp_fixture, "checkpoint selection picks highest timestamp at or below ckpt ts", "[key_provider]")
{
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_KEY_PROVIDER stub = {};
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=1") == 0);

    /* Each pushed key has distinct bytes so the chosen entry's key can be validated. */
    const std::string keys[4] = {
      "selection-key-ten", "selection-key-twenty", "selection-key-thirty", "selection-key-forty"};
    const uint64_t timestamps[4] = {10, 20, 30, 40};
    for (int i = 0; i < 4; i++)
        REQUIRE(push_key(&stub, keys[i], timestamps[i]) == 0);

    SECTION("a timestamp that straddles the queue picks the highest entry not exceeding it")
    {
        validate_chosen_key(__ut_disagg_select_pending_crypt_key(session_impl, 25), 20, keys[1]);
    }

    SECTION("an exact match on a queued timestamp is selected")
    {
        validate_chosen_key(__ut_disagg_select_pending_crypt_key(session_impl, 30), 30, keys[2]);
    }

    SECTION("a timestamp beyond the queue selects the newest entry")
    {
        validate_chosen_key(__ut_disagg_select_pending_crypt_key(session_impl, 100), 40, keys[3]);
    }

    SECTION("a timestamp before the whole queue selects nothing")
    {
        REQUIRE(__ut_disagg_select_pending_crypt_key(session_impl, 5) == nullptr);
    }

    __wti_disagg_pending_crypt_key_clear(session_impl);
    conn_impl->key_provider = nullptr;
    F_CLR(conn_impl, WT_CONN_KEY_PROVIDER_PUSH);
}

TEST_CASE_METHOD(kp_fixture, "checkpoint prune scenarios", "[key_provider]")
{
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_KEY_PROVIDER stub = {};
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=1") == 0);

    /* Pruning an empty queue is a no-op. */
    __ut_disagg_prune_pending_crypt_keys(session_impl, 100);
    validate_pending_queue(conn_impl, {});

    const std::string keys[3] = {"prune-key-ten", "prune-key-twenty", "prune-key-thirty"};
    const uint64_t timestamps[3] = {10, 20, 30};
    for (int i = 0; i < 3; i++)
        REQUIRE(push_key(&stub, keys[i], timestamps[i]) == 0);

    SECTION("a bound below every entry frees nothing")
    {
        __ut_disagg_prune_pending_crypt_keys(session_impl, 5);
        validate_pending_queue(conn_impl, {{10, keys[0]}, {20, keys[1]}, {30, keys[2]}});
    }

    SECTION("a bound exactly at an entry frees that entry and everything older")
    {
        __ut_disagg_prune_pending_crypt_keys(session_impl, 20);
        validate_pending_queue(conn_impl, {{30, keys[2]}});
    }

    SECTION("a bound between entries frees the covered keys and retains the newer one")
    {
        __ut_disagg_prune_pending_crypt_keys(session_impl, 25);
        validate_pending_queue(conn_impl, {{30, keys[2]}});
    }

    SECTION("a bound at or above every entry drains the whole queue")
    {
        __ut_disagg_prune_pending_crypt_keys(session_impl, 1000);
        validate_pending_queue(conn_impl, {});
    }

    __wti_disagg_pending_crypt_key_clear(session_impl);
    conn_impl->key_provider = nullptr;
    F_CLR(conn_impl, WT_CONN_KEY_PROVIDER_PUSH);
}

TEST_CASE_METHOD(kp_fixture, "checkpoint selection edge cases", "[key_provider]")
{
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_KEY_PROVIDER stub = {};
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=1") == 0);

    /* An empty queue selects nothing. */
    REQUIRE(__ut_disagg_select_pending_crypt_key(session_impl, 100) == nullptr);

    /* A single entry is selected only once the checkpoint timestamp reaches it. */
    const std::string edge_key = "edge-test-key";
    REQUIRE(push_key(&stub, edge_key, 50) == 0);
    REQUIRE(__ut_disagg_select_pending_crypt_key(session_impl, 49) == nullptr);
    validate_chosen_key(__ut_disagg_select_pending_crypt_key(session_impl, 50), 50, edge_key);

    __wti_disagg_pending_crypt_key_clear(session_impl);
    conn_impl->key_provider = nullptr;
    F_CLR(conn_impl, WT_CONN_KEY_PROVIDER_PUSH);
}

TEST_CASE_METHOD(kp_fixture, "pending key clear drains the whole queue", "[key_provider]")
{
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_KEY_PROVIDER stub = {};
    REQUIRE(wt_conn->set_key_provider(wt_conn, &stub, "version=1") == 0);

    const std::string keys[3] = {"clear-key-ten", "clear-key-twenty", "clear-key-thirty"};
    const uint64_t timestamps[3] = {10, 20, 30};
    for (int i = 0; i < 3; i++)
        REQUIRE(push_key(&stub, keys[i], timestamps[i]) == 0);
    validate_pending_queue(conn_impl, {{10, keys[0]}, {20, keys[1]}, {30, keys[2]}});

    /* Clearing drains every queued key regardless of timestamp. */
    __wti_disagg_pending_crypt_key_clear(session_impl);
    validate_pending_queue(conn_impl, {});

    conn_impl->key_provider = nullptr;
    F_CLR(conn_impl, WT_CONN_KEY_PROVIDER_PUSH);
}

TEST_CASE_METHOD(kp_fixture, "Key always expires", "[key_provider]")
{
    kp_ptr_t kp = kp_init("key_expires=0");
    REQUIRE(kp->wtext != nullptr);

    REQUIRE(kp->verbose == WT_VERBOSE_INFO);
    REQUIRE(kp->key_expires == 0);
    REQUIRE(kp->state.key_state == KEY_STATE_CURRENT);

    const auto initial_key_time = kp->state.key_time;

    WT_KEY_PROVIDER *wtkp = &kp->iface;

    /* Probe the key; the key is always rotated */
    WT_CRYPT_KEYS crypt = kp_get_key();
    const auto first_key_time = kp->state.key_time;
    REQUIRE(initial_key_time != first_key_time); /* New key generated */

    /* Simulate key queueing */
    const uint64_t new_lsn = 84; /* New LSN after queueing */
    crypt.r.lsn = new_lsn;
    REQUIRE(wtkp->on_key_update(wtkp, session, &crypt) == 0);
    REQUIRE(kp->state.key_state == KEY_STATE_CURRENT);
    /* LSN should be updated on success */
    REQUIRE(kp->state.lsn == new_lsn);
    free(const_cast<void *>(crypt.keys.data));

    /* Probe the key again; the key is always rotated */
    crypt = kp_get_key();
    REQUIRE(first_key_time != kp->state.key_time); /* New key generated */
    /* New key is not queued yet */
    REQUIRE(kp->state.lsn == 0);
    free(const_cast<void *>(crypt.keys.data));
}
