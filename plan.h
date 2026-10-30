/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file plan.h
 * @brief Checked address-placement primitives shared by the module and CTest.
 *
 * All ranges use inclusive endpoints. These helpers allocate no storage and
 * never modify caller-owned interval lists. Failure is reported before any
 * kernel resource or PCI register is changed by the calling transaction.
 */
#ifndef PEX_WINDOW_PLAN_H
#define PEX_WINDOW_PLAN_H
#ifdef __KERNEL__
#include <linux/types.h>
/** @brief Unsigned 64-bit arithmetic in kernel builds. */
typedef u64 pw_u64;
#else
#include <stdint.h>
/** @brief Unsigned 64-bit arithmetic in userspace tests. */
typedef uint64_t pw_u64;
#endif
/** @brief One binary gibibyte, expressed as an unsigned 64-bit value. */
#define PW_GIB ((pw_u64)1 << 30)
/** @brief Default per-GPU reservation used by the test fixtures. */
#define PW_SLOT_BYTES (64 * PW_GIB)
/** @brief An inclusive interval occupied by an immovable resource or a new plan. */
struct pw_interval {
    pw_u64 start; /**< Inclusive first occupied address. */
    pw_u64 end;   /**< Inclusive last occupied address. */
};

/**
 * @brief Place one aligned allocation without wrapping unsigned arithmetic.
 * @param cursor Earliest permissible base address.
 * @param limit Inclusive aperture limit.
 * @param size Requested bytes; must be nonzero.
 * @param alignment Required power-of-two alignment.
 * @param start Output inclusive base, valid on success.
 * @param end Output inclusive limit, valid on success.
 * @return Zero on success; -1 for invalid arguments, overflow, or insufficient space.
 */
static inline int pw_place(pw_u64 cursor, pw_u64 limit, pw_u64 size, pw_u64 alignment,
                           pw_u64 *start, pw_u64 *end)
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

/**
 * @brief Test whether two valid inclusive intervals intersect.
 * @param a First interval's base.
 * @param b First interval's limit, at least a.
 * @param c Second interval's base.
 * @param d Second interval's limit, at least c.
 * @return Nonzero for overlap, including a shared endpoint; zero otherwise.
 */
static inline int pw_overlap(pw_u64 a, pw_u64 b, pw_u64 c, pw_u64 d)
{
    return a <= d && c <= b;
}

/**
 * @brief Compute aggregate slot capacity without a fixed GPU-count ceiling.
 * @param slots Number of descendant GPU or reserved empty slots, nonzero.
 * @param slot_bytes Reservation per slot in bytes, nonzero.
 * @param bytes Output aggregate size, valid on success.
 * @return Zero on success; -1 for zero capacity or multiplication overflow.
 */
static inline int pw_slot_span(pw_u64 slots, pw_u64 slot_bytes, pw_u64 *bytes)
{
    if (!slots || !slot_bytes || slots > ~(pw_u64)0 / slot_bytes)
        return -1;
    *bytes = slots * slot_bytes;
    return 0;
}

/**
 * @brief Find the first aligned gap large enough in a host aperture.
 * @param first Inclusive host-aperture base.
 * @param last Inclusive host-aperture limit.
 * @param bytes Requested nonzero contiguous allocation size.
 * @param alignment Required power-of-two base alignment.
 * @param used Unsorted occupied intervals; may be NULL when count is zero.
 * @param count Number of occupied intervals.
 * @param start Output inclusive allocation base, valid on success.
 * @param end Output inclusive allocation limit, valid on success.
 * @return Zero on success; -1 for invalid arithmetic or no suitable gap.
 *
 * Callers include previous plans and immovable host reservations, excluding
 * old allocations that the same transaction will release. Overlapping occupied
 * intervals are supported. Every conflict advances the cursor monotonically.
 */
static inline int pw_find_space(pw_u64 first, pw_u64 last, pw_u64 bytes, pw_u64 alignment,
                                const struct pw_interval *used, unsigned int count, pw_u64 *start,
                                pw_u64 *end)
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

/**
 * @brief Check an original range is still free before dismantling an applied layout.
 * @param start Original inclusive range base.
 * @param end Original inclusive range limit.
 * @param parent_start Parent aperture base after restoration.
 * @param parent_end Parent aperture limit after restoration.
 * @param used Immutable current allocations; moving resources are excluded.
 * @param count Number of immutable allocations.
 * @return Zero when contained and conflict-free; -1 otherwise.
 */
static inline int pw_original_range_free(pw_u64 start, pw_u64 end, pw_u64 parent_start,
                                         pw_u64 parent_end, const struct pw_interval *used,
                                         unsigned int count)
{
    unsigned int i;
    if (start > end || start < parent_start || end > parent_end)
        return -1;
    for (i = 0; i < count; i++)
        if (used[i].start > used[i].end || pw_overlap(start, end, used[i].start, used[i].end))
            return -1;
    return 0;
}
#endif
