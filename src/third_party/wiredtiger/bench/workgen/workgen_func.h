/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
struct workgen_random_state;

extern uint32_t workgen_atomic_add32(uint32_t *vp, uint32_t v);
extern uint64_t workgen_atomic_add64(uint64_t *vp, uint64_t v);
extern uint32_t workgen_atomic_sub32(uint32_t *vp, uint32_t v);
extern void workgen_clock(uint64_t *tsp);
extern void workgen_epoch(struct timespec *tsp);
extern uint32_t workgen_random(struct workgen_random_state volatile *rnd_state);
extern int workgen_random_alloc(WT_SESSION *session, struct workgen_random_state **rnd_state);
extern void workgen_random_free(struct workgen_random_state *rnd_state);
extern void workgen_u64_to_string_zf(uint64_t n, char *buf, size_t len);
extern void workgen_version(char *buf, size_t len);
