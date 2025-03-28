/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Test the live restore bitmap encode and decode functionality. [live_restore_bitmap]
 */

#include "../utils_live_restore.h"
#include "../../wrappers/mock_session.h"
#include "../../wrappers/item_wrapper.h"

using namespace utils;

struct test_data {
    test_data(std::string bitmap_str, uint64_t nbits, uint8_t *bitmap)
        : bitmap_str(bitmap_str), nbits(nbits), bitmap(bitmap)
    {
    }

    std::string bitmap_str;
    uint64_t nbits;
    uint8_t *bitmap;
};

TEST_CASE("Encode various bitmaps", "[live_restore_bitmap],[live_restore_bitmap_encode_decode]")
{
    std::shared_ptr<mock_session> mock_session = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock_session->get_wt_session_impl();

    test_data test1 = test_data("00", 8, new uint8_t[1]{0x0});
    test_data test2 = test_data("ab", 8, new uint8_t[1]{0xab});
    test_data test3 = test_data("11", 8, new uint8_t[1]{0x11});
    test_data test4 = test_data("0000", 16, new uint8_t[2]{0x0, 0x0});
    test_data test5 = test_data("000102", 24, new uint8_t[3]{0x0, 0x1, 0x2});
    test_data test6 = test_data("0000", 9, new uint8_t[2]{0x0, 0x0});
    test_data test7 = test_data("0004", 9, new uint8_t[2]{0x0, 0x4});
    test_data test8 = test_data("0400", 15, new uint8_t[2]{0x4, 0x0});
    test_data test9 = test_data("", 0, nullptr);
    std::vector<test_data> test_bitmaps = {
      test1, test2, test3, test4, test5, test6, test7, test8, test9};

    WT_ITEM buf;
    WT_CLEAR(buf);

    live_restore_test_env env;
    for (const auto &test : test_bitmaps) {
        size_t filesize = test.nbits == 0 ? 4096 : 4096 * test.nbits;
        std::string dest_file = env.dest_file_path("file");
        std::string source_file = env.source_file_path("file");
        create_file(source_file, filesize);

        WT_FILE_HANDLE *fh;
        REQUIRE(env.lr_fs->iface.fs_open_file((WT_FILE_SYSTEM *)env.lr_fs,
                  reinterpret_cast<WT_SESSION *>(env.session), dest_file.c_str(),
                  WT_FS_OPEN_FILE_TYPE_DATA, 0, &fh) == 0);
        WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;

        REQUIRE(testutil_exists(".", dest_file.c_str()));

        lr_fh->bitmap = test.bitmap;
        lr_fh->nbits = test.nbits;

        __wt_readlock(session, &lr_fh->lock);
        REQUIRE(__ut_live_restore_encode_bitmap(session, lr_fh, &buf) == 0);
        __wt_readunlock(session, &lr_fh->lock);
        // In the live restore code we only call decode if nbits is not zero.
        if (test.nbits != 0) {
            REQUIRE(
              std::string(static_cast<const char *>(buf.data)) == std::string(test.bitmap_str));
            lr_fh->allocsize = 4096;
            REQUIRE(__ut_live_restore_decode_bitmap(
                      session, test.bitmap_str.c_str(), test.nbits, lr_fh) == 0);
        }
        if (test.nbits != 0)
            REQUIRE(memcmp(lr_fh->bitmap, test.bitmap, test.nbits / 8) == 0);

        lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)env.session);
        testutil_remove(dest_file.c_str());
        testutil_remove(source_file.c_str());

        delete[] test.bitmap;
        __wt_buf_free(session, &buf);
        WT_CLEAR(buf);
    }
}
