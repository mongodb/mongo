#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.

import logging
import sqlite3
import sys

from typing import Iterable, List, Optional

from py_common import disagg
from py_common.decode_opts import DecodeOptions


SQLITE3_SIGNATURE = b'SQLite format 3\x00'
WT_PAGE_LOG_DELTA = 0x2

logger = logging.getLogger(__name__)


def is_sqlite3_file(filename: str) -> bool:
    if filename == '-':
        return False

    with open(filename, 'rb') as infile:
        return infile.read(len(SQLITE3_SIGNATURE)) == SQLITE3_SIGNATURE


def _load_rows_for_page(
    conn: sqlite3.Connection,
    page_id: int
) -> Optional[List[sqlite3.Row]]:
    # First check for multiple base pages for the page_id.
    rows = list(
        conn.execute(
            '''
            SELECT page_id, lsn
              FROM pages
             WHERE page_id = ?
               AND base_lsn = 0
               AND backlink_lsn = 0
               AND discarded = 0
            ORDER BY lsn DESC
            ''',
            (page_id,),
        )
    )

    if not rows:
        return None

    if len(rows) > 1:
        logger.warning(
            f'Found {len(rows)} base pages for page_id={page_id}. '
            f'Selecting newest base lsn={rows[0]["lsn"]}; '
            f'ignoring older base lsns={[row["lsn"] for row in rows[1:]]}',
        )

    # Select the newest base lsn.
    base_lsn = rows[0]['lsn']

    return list(
        conn.execute(
            '''
            SELECT table_id,
                   page_id,
                   lsn,
                   backlink_lsn,
                   base_lsn,
                   flags,
                   page_data
              FROM pages
             WHERE page_id = ?
               AND lsn >= ?
               AND discarded = 0
             ORDER BY lsn DESC
            ''',
            (page_id, base_lsn),
        )
    )


def _load_row_for_lsn(conn: sqlite3.Connection, lsn: int) -> Optional[sqlite3.Row]:
    return conn.execute(
        '''
        SELECT table_id,
               page_id,
               lsn,
               backlink_lsn,
               base_lsn,
               flags,
               page_data
          FROM pages
         WHERE lsn = ?
           AND discarded = 0
        ''',
        (lsn,),
    ).fetchone()


def _rows_to_disagg_pages(rows: Iterable[sqlite3.Row]) -> List[disagg.DisaggPage]:
    disagg_pages: List[disagg.DisaggPage] = []
    for row in rows:
        flags = row['flags']
        metadata = disagg.Metadata(
            lsn=row['lsn'],
            page_id=row['page_id'],
            table_id=row['table_id'],
            base_lsn=row['base_lsn'],
            backlink_lsn=row['backlink_lsn'],
            delta=(flags & WT_PAGE_LOG_DELTA) != 0,
        )
        page_data = row['page_data']
        if page_data is None:
            page_data = b''
        disagg_pages.append(disagg.DisaggPage(metadata, page_data))
    return disagg_pages


def _load_disagg_pages_all(
    conn: sqlite3.Connection,
    pages_limit: int
) -> List[List[disagg.DisaggPage]]:
    logger.info(
        f'No page_id or lsn specified; loading all pages up to the limit of {pages_limit}'
    )

    # Select all known page ids.
    page_ids = conn.execute(
        '''
        SELECT DISTINCT page_id
          FROM pages
         WHERE discarded = 0
        '''
    ).fetchall()

    disagg_pages = []
    page_count = 0
    for (page_id,) in page_ids:
        rows = _load_rows_for_page(conn, page_id)
        if rows is not None:
            disagg_pages.append(_rows_to_disagg_pages(rows))
            page_count += len(rows)
        if page_count >= pages_limit:
            logger.info(f'Loaded {page_count} pages, reaching the specified limit')
            break

    return disagg_pages


def _load_disagg_pages_for_lsn(
    conn: sqlite3.Connection,
    lsn: int,
    page_id: Optional[int],
) -> List[List[disagg.DisaggPage]]:
    row = _load_row_for_lsn(conn, lsn)
    if row is None:
        raise ValueError(f'No page found for lsn={lsn}')
    if page_id is not None and row['page_id'] != page_id:
        raise ValueError(
            f'No page found for page_id={page_id}, lsn={lsn}'
        )
    return [_rows_to_disagg_pages([row])]


def _load_disagg_pages_for_page_id(
    conn: sqlite3.Connection,
    page_id: int,
) -> List[List[disagg.DisaggPage]]:
    rows = _load_rows_for_page(conn, page_id)
    if not rows:
        raise ValueError(f'No page found for page_id={page_id}')
    return [_rows_to_disagg_pages(rows)]


def load_disagg_pages(filename: str, *, lsn=None, page_id=None, pages: int = 0) -> List[List[disagg.DisaggPage]]:
    with sqlite3.connect(filename) as conn:
        conn.row_factory = sqlite3.Row

        # Specific lsn takes precedence over page_id selection,
        # since it provides a more precise selection of the page to decode.
        if lsn is not None:
            return _load_disagg_pages_for_lsn(conn, lsn, page_id)

        # Select entire page chain for given page_id.
        if page_id is not None:
            return _load_disagg_pages_for_page_id(conn, page_id)

        # No specific selection criteria provided; return all pages up to the limit.
        pages_limit = pages if pages > 0 else sys.maxsize
        return _load_disagg_pages_all(conn, pages_limit)


def process_sqlite_file(filename: str, opts: DecodeOptions) -> disagg.DisaggTableSummary:
    disagg_pages = load_disagg_pages(filename, lsn=opts.lsn, page_id=opts.page_id, pages=opts.pages)
    return disagg.process_disagg_pages(disagg_pages, opts)
