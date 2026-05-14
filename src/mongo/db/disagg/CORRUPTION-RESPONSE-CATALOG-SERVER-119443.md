# Corruption Response Catalog for Disaggregated Storage

Ticket: SERVER-119443 (sesi-disagg + sesi-errors). PALI/WT today treat all detected
corruption as fatal. Disaggregated storage exposes several distinct detection
surfaces with different blast radii; one-size-fits-all fatal is both too aggressive
(for surfaces with a clean retry path) and too lax (for surfaces where a quiet
fallback would replicate poison). This catalog fixes one response per surface, the
invariants that bound future changes, and the test matrix pinning each surface.

## 1. Detection Surfaces

Five surfaces, each with a different scope (page / segment / object), repair path
(local re-read, peer re-read, object-store re-fetch), and transient-vs-persistent
signal ratio.

- **S1 — Page checksum mismatch.** Detected by WT page-load on read; scope is one
  page within one segment. Often transient (bit-flip in transit or in page cache);
  sometimes persistent (on-disk rot or bad object).
- **S2 — Log entry CRC mismatch.** Detected by the disagg log replay path; scope is
  one log record. A mismatch at the tail is normally truncation, not corruption; a
  mismatch in the interior is structural and unrecoverable in place.
- **S3 — Object-store ETag mismatch.** Detected on `GET` after a `PUT` round-trip;
  scope is one whole object. Indicates either a stale object-store cache or a
  mid-flight rewrite by a non-cooperating writer.
- **S4 — Decryption tag mismatch.** Detected by the AEAD layer; scope is one
  ciphertext blob. Cannot be retried in place — by construction the plaintext is
  unrecoverable from that ciphertext.
- **S5 — Structural BSON invalid.** Detected post-decrypt, post-checksum, by the
  BSON validator. Scope is one document. Indicates a logic bug or an undetected
  upstream surface failure (the lower layers passed; the bytes are still wrong).

## 2. Recommended Response

One response policy per surface. Categories are **fatal**, **quarantine-and-retry-
elsewhere**, and **structured-error-to-client**.

- **S1 page checksum → quarantine-and-retry-elsewhere.** Mark the page bad in the
  local cache, increment `disagg.corruption.page_checksum`, re-fetch from a peer
  replica or from object-store cold tier. Only escalate to fatal if N consecutive
  re-fetches in a bounded window also fail (defaults to 3 / 60s).
- **S2 log CRC → fatal if interior, structured-error if tail.** Tail CRC mismatch
  is normal recovery truncation. Interior CRC mismatch means the durable log is
  inconsistent with itself; downstream replay would produce divergent state.
- **S3 object-store ETag → quarantine-and-retry-elsewhere.** Re-fetch with explicit
  `If-Match` against the catalog-recorded ETag; if the second fetch also mismatches,
  treat as object loss and trigger a re-replication from peers. Never serve the
  mismatched bytes.
- **S4 decryption tag → structured-error-to-client.** Surface an `IncompatibleData`
  / `DataCorruptionDetected` error with object key and offset. Do not retry locally
  (the plaintext is gone). Operator must restore from backup or re-replicate.
- **S5 structural BSON invalid → fatal (configurable to structured-error).** Default
  fatal because S5 indicates either a logic bug or an upstream surface that
  incorrectly passed. Configurable to structured-error for forensic recovery on a
  quiesced node.

## 3. Invariants

Any future change to surface-level policy MUST preserve all three.

1. **Never silently swallow corruption.** Every detection on every surface
   increments a named counter and emits a log line with object key, offset, and
   surface tag. A response of "retry elsewhere" is still an observable event.
2. **Never re-replicate corrupted bytes.** A page or object that failed S1/S3/S4
   MUST NOT be the source for any outbound replication, re-shard, or backup write.
   Quarantine markers participate in replication source selection.
3. **Always surface to observability.** Each surface has a named counter, a log tag,
   and an FTDC field, suitable for ingest by the standard server diagnostics pipeline
   and operator alerting.

## 4. Test Matrix

One row per surface × response category. Each row is a deterministic fault-injection
jstest plus a unit test pinning the dispatch.

| Surface | Injection                            | Response                | Test                                          |
|---------|--------------------------------------|-------------------------|-----------------------------------------------|
| S1      | flip bit in cached page, force read  | retry-elsewhere, metric | `disagg_page_checksum_retry.js` + unit        |
| S1 (N)  | inject N consecutive bad re-fetches  | fatal, crash log line   | `disagg_page_checksum_fatal.js`               |
| S2 tail | truncate last log record mid-CRC     | structured-error, OK    | `disagg_log_tail_truncation.js`               |
| S2 mid  | flip bit in interior log CRC         | fatal                   | `disagg_log_interior_crc_fatal.js`            |
| S3      | rewrite object out-of-band           | retry-elsewhere, then   | `disagg_objstore_etag_mismatch.js`            |
|         |                                      | re-replicate            |                                               |
| S4      | flip AEAD tag                        | structured-error        | `disagg_aead_tag_mismatch.js`                 |
| S5      | inject invalid BSON post-decrypt     | fatal (default)         | `disagg_bson_structural_fatal.js`             |
| S5 cfg  | same, with `forensicRecovery=true`   | structured-error        | `disagg_bson_structural_forensic.js`          |

Each test asserts: (a) the correct response category fires; (b) the named metric
increments; (c) no quarantined byte appears on any outbound replication channel; (d)
the corresponding log line and FTDC field are emitted exactly once per event.
