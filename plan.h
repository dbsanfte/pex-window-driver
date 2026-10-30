/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEX_WINDOW_PLAN_H
#define PEX_WINDOW_PLAN_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 pw_u64;
#else
#include <stdint.h>
typedef uint64_t pw_u64;
#endif
#define PW_GIB ((pw_u64)1 << 30)
#define PW_SLOT_BYTES (64 * PW_GIB)
struct pw_interval { pw_u64 start, end; };

/* Inclusive limits throughout. No arithmetic may wrap at the address limit. */
static inline int pw_place(pw_u64 cursor, pw_u64 limit, pw_u64 size,
                          pw_u64 alignment, pw_u64 *start, pw_u64 *end)
{
    pw_u64 mask;
    if (!size || !alignment || (alignment & (alignment - 1)))
        return -1;
    mask = alignment - 1;
    if (cursor > ~(pw_u64)0 - mask)
        return -1;
    *start = (cursor + mask) & ~mask;
    if (*start > limit || size - 1 > limit - *start)
        return -1;
    *end = *start + size - 1;
    return 0;
}

static inline int pw_overlap(pw_u64 a, pw_u64 b, pw_u64 c, pw_u64 d)
{
    return a <= d && c <= b;
}

static inline int pw_slot_span(pw_u64 slots, pw_u64 slot_bytes, pw_u64 *bytes)
{
    if (!slots || !slot_bytes || slots > ~(pw_u64)0 / slot_bytes)
        return -1;
    *bytes = slots * slot_bytes;
    return 0;
}

/* First fit in a host aperture. Occupied intervals need not be sorted.
 * Callers include previous plans as well as immovable host reservations.
 * Firmware allocations being moved by this transaction are excluded.
 */
static inline int pw_find_space(pw_u64 first, pw_u64 last, pw_u64 bytes,
                                pw_u64 alignment, const struct pw_interval *used,
                                unsigned int count, pw_u64 *start, pw_u64 *end)
{
    pw_u64 cursor = first;
    for (;;) {
        unsigned int i;
        if (pw_place(cursor, last, bytes, alignment, start, end))
            return -1;
        for (i = 0; i < count; ++i) {
            if (used[i].start > used[i].end)
                return -1;
            if (!pw_overlap(*start, *end, used[i].start, used[i].end))
                continue;
            if (used[i].end == ~(pw_u64)0)
                return -1;
            cursor = used[i].end + 1;
            break;
        }
        if (i == count)
            return 0;
    }
}
#endif
