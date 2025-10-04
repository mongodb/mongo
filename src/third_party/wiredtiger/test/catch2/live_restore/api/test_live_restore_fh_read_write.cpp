/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Test the live restore __live_restore_fh_read, __live_restore_fh_write functionality.
 * [live_restore_fh_read_write].
 */

#include "../utils_live_restore.h"

using namespace utils;

static void
init_file_handle(WT_SESSION *session, WTI_LIVE_RESTORE_FS *lr_fs, const char *file_name,
  uint32_t allocsize, wt_off_t file_size, WTI_LIVE_RESTORE_FILE_HANDLE **lr_fhp)
{
    testutil_check(lr_fs->iface.fs_open_file((WT_FILE_SYSTEM *)lr_fs, session, file_name,
      WT_FS_OPEN_FILE_TYPE_DATA, WT_FS_OPEN_CREATE, (WT_FILE_HANDLE **)lr_fhp));

    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = *lr_fhp;
    lr_fh->allocsize = allocsize;
    lr_fh->nbits = (uint64_t)file_size / allocsize;
    testutil_check(__bit_alloc((WT_SESSION_IMPL *)session, lr_fh->nbits, &lr_fh->bitmap));
}

TEST_CASE("Live Restore fh_read fh_write", "[live_restore],[live_restore_fh_read_write]")
{
    auto file_name = "test_table.wt";
    // Select a file_size that is not divisible by page_size, so we can test a write where the first
    // portion falls within the bitmap and the second portion exceeds it.
    uint32_t allocsize = 4, page_size = allocsize * 4;
    wt_off_t file_size = (wt_off_t)allocsize * 33;
    testutil_assert(file_size % page_size != 0);
    auto dummy_char = '0';
    auto source_char = '1';
    auto write_char = '2';

    live_restore_test_env env;
    WT_SESSION *session = reinterpret_cast<WT_SESSION *>(env.session);
    WTI_LIVE_RESTORE_FS *lr_fs = env.lr_fs;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = nullptr;

    SECTION("Read/write when source exists")
    {
        create_file(env.source_file_path(file_name), file_size, source_char);
        // Create a dest file, pre-filled dummy chars in the dest file are used to verify we are not
        // touching them when their corresponding bits are set in the bitmap.
        create_file(env.dest_file_path(file_name), file_size, dummy_char);

        init_file_handle(
          session, lr_fs, env.dest_file_path(file_name).c_str(), allocsize, file_size, &lr_fh);
        std::vector<char> read_buf(page_size);

        // No writes yet, reads go into source.
        std::vector<char> data(page_size, source_char);
        testutil_check(
          lr_fh->iface.fh_read((WT_FILE_HANDLE *)lr_fh, session, 0, page_size, read_buf.data()));
        REQUIRE(read_buf == data);

        /*
         * Background migration in progress, simulate this by calling a fh_write() to write source
         * chars to dest, where the first few pages are fully migrated and the last page is
         * partially migrated.
         */
        auto count = 13;
        // Select a count value that formulates background_write_len to be non-divisible by
        // page_size to simulate a page being partially migrated.
        auto background_write_len = allocsize * count;
        testutil_assert(background_write_len <= file_size);
        testutil_assert(background_write_len % page_size != 0);
        std::vector<char> write_buf(background_write_len, source_char);
        testutil_check(lr_fh->iface.fh_write(
          (WT_FILE_HANDLE *)lr_fh, session, 0, background_write_len, write_buf.data()));

        // The first floor(migration_page_count) are fully migrated and the data should have been
        // copied to the dest file.
        int offset = 0;
        for (; offset + page_size < background_write_len; offset += page_size) {
            testutil_check(lr_fh->iface.fh_read(
              (WT_FILE_HANDLE *)lr_fh, session, offset, page_size, read_buf.data()));
            REQUIRE(read_buf == data);
            // Verify data has been written to the dest file.
            testutil_check(lr_fh->destination->fh_read(
              lr_fh->destination, session, offset, page_size, read_buf.data()));
            REQUIRE(read_buf == data);
        }

        // The last page is partially migrated, since there's no write on dest yet read will just
        // get source chars.
        testutil_check(lr_fh->iface.fh_read(
          (WT_FILE_HANDLE *)lr_fh, session, offset, page_size, read_buf.data()));
        REQUIRE(read_buf == data);
        // However in the dest file we should only see the first part of the page copied where the
        // second part should still be dummy chars.
        testutil_check(lr_fh->destination->fh_read(
          lr_fh->destination, session, offset, page_size, read_buf.data()));
        auto migrated_size = background_write_len % page_size;
        std::fill(data.begin(), data.begin() + migrated_size, source_char);
        std::fill(data.begin() + migrated_size, data.end(), dummy_char);
        REQUIRE(read_buf == data);

        // Test write happens after migration, this should overwrite data in dest where source
        // remains unchanged.
        std::fill(data.begin(), data.end(), write_char);
        write_buf.assign(page_size, write_char);
        testutil_check(
          lr_fh->iface.fh_write((WT_FILE_HANDLE *)lr_fh, session, 0, page_size, write_buf.data()));
        testutil_check(
          lr_fh->iface.fh_read((WT_FILE_HANDLE *)lr_fh, session, 0, page_size, read_buf.data()));
        REQUIRE(read_buf == data);
        testutil_check(
          lr_fh->destination->fh_read(lr_fh->destination, session, 0, page_size, read_buf.data()));
        REQUIRE(read_buf == data);
        std::fill(data.begin(), data.end(), source_char);
        testutil_check(
          lr_fh->source->fh_read(lr_fh->source, session, 0, page_size, read_buf.data()));
        REQUIRE(read_buf == data);

        // Test a write that partially exceeds the bitmap.
        std::fill(data.begin(), data.end(), write_char);
        std::fill(write_buf.begin(), write_buf.end(), write_char);
        offset = file_size / page_size * page_size;
        testutil_check(lr_fh->iface.fh_write(
          (WT_FILE_HANDLE *)lr_fh, session, offset, page_size, write_buf.data()));
        testutil_check(lr_fh->iface.fh_read(
          (WT_FILE_HANDLE *)lr_fh, session, offset, page_size, read_buf.data()));
        REQUIRE(read_buf == data);
        testutil_check(lr_fh->destination->fh_read(
          lr_fh->destination, session, offset, page_size, read_buf.data()));
        REQUIRE(read_buf == data);

        // Test a write that goes completely beyond the bitmap range.
        std::fill(write_buf.begin(), write_buf.end(), write_char);
        offset = (file_size / page_size + 5) * page_size;
        testutil_check(lr_fh->iface.fh_write(
          (WT_FILE_HANDLE *)lr_fh, session, offset, page_size, write_buf.data()));
        testutil_check(lr_fh->iface.fh_read(
          (WT_FILE_HANDLE *)lr_fh, session, offset, page_size, read_buf.data()));
        REQUIRE(read_buf == data);
        testutil_check(lr_fh->destination->fh_read(
          lr_fh->destination, session, offset, page_size, read_buf.data()));
        REQUIRE(read_buf == data);

        // Source should remain untouched during the whole test.
        data.assign(file_size, source_char);
        read_buf.resize(file_size);
        testutil_check(
          lr_fh->source->fh_read(lr_fh->source, session, 0, file_size, read_buf.data()));
        REQUIRE(read_buf == data);

        testutil_check(lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, session));
    }

    SECTION("Read/write when source doesn't exist")
    {
        testutil_remove(env.source_file_path(file_name).c_str());
        testutil_remove(env.dest_file_path(file_name).c_str());
        create_file(env.dest_file_path(file_name), file_size, dummy_char);

        init_file_handle(
          session, lr_fs, env.dest_file_path(file_name).c_str(), allocsize, file_size, &lr_fh);

        auto data = std::vector<char>(page_size, write_char);
        std::vector<char> read_buf(page_size);
        std::vector<char> write_buf(page_size, write_char);
        // Test written data can be read from fh_read().
        testutil_check(
          lr_fh->iface.fh_write((WT_FILE_HANDLE *)lr_fh, session, 0, page_size, write_buf.data()));
        testutil_check(
          lr_fh->iface.fh_read((WT_FILE_HANDLE *)lr_fh, session, 0, page_size, read_buf.data()));
        REQUIRE(read_buf == data);
        // Verify data has been written to the dest file.
        testutil_check(
          lr_fh->destination->fh_read(lr_fh->destination, session, 0, page_size, read_buf.data()));
        REQUIRE(read_buf == data);

        // Test a write that partially exceeds the bitmap.
        auto offset = file_size / page_size * page_size;
        testutil_check(lr_fh->iface.fh_write(
          (WT_FILE_HANDLE *)lr_fh, session, offset, page_size, write_buf.data()));
        testutil_check(lr_fh->iface.fh_read(
          (WT_FILE_HANDLE *)lr_fh, session, offset, page_size, read_buf.data()));
        REQUIRE(read_buf == data);
        testutil_check(lr_fh->destination->fh_read(
          lr_fh->destination, session, offset, page_size, read_buf.data()));
        REQUIRE(read_buf == data);

        // Test a write that goes completely beyond the bitmap range.
        offset = (file_size / page_size + 5) * page_size;
        testutil_check(lr_fh->iface.fh_write(
          (WT_FILE_HANDLE *)lr_fh, session, offset, page_size, write_buf.data()));
        testutil_check(lr_fh->iface.fh_read(
          (WT_FILE_HANDLE *)lr_fh, session, offset, page_size, read_buf.data()));
        REQUIRE(read_buf == data);
        testutil_check(lr_fh->destination->fh_read(
          lr_fh->destination, session, offset, page_size, read_buf.data()));
        REQUIRE(read_buf == data);

        // Source should remain untouched during the whole test.
        REQUIRE(lr_fh->source == nullptr);

        testutil_check(lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, session));
    }
}
