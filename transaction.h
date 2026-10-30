/* SPDX-License-Identifier: GPL-2.0-only */
/** @file transaction.h
 * @brief Hardware-independent transaction ordering, used by the driver and CTest.
 *
 * Backends supply PCI/resource operations. These helpers enforce the same
 * disable/drain/update/readback/recovery sequence in production and unit tests.
 */
#ifndef PEX_WINDOW_TRANSACTION_H
#define PEX_WINDOW_TRANSACTION_H
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
/** @brief Configuration-word type in kernel builds. */
typedef u16 pw_u16;
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
/** @brief Configuration-word type in userspace tests. */
typedef uint16_t pw_u16;
#endif

/** @brief PCI and resource operations injected into a transaction. */
struct pw_operations {
    int (*commands)(void *, bool); /**< Disable or restore command registers, with readback. */
    int (*drain)(void *);          /**< Wait for all pending endpoint transactions. */
    int (*release)(void *);        /**< Release children before parents. */
    int (*register_layout)(void *, bool); /**< Register original (true) or new resources. */
    int (*program)(void *, bool); /**< Program/read back original (true) or new registers. */
    int (*save)(void *);          /**< Refresh PCI-core power-management snapshots. */
};
/** @brief Application outcome, including recovery and containment results. */
struct pw_outcome {
    int recovery_error; /**< Recovery error, zero if recovery succeeded/not needed. */
    int disable_error;  /**< Last containment disable result. */
    bool containment;   /**< An error required a final disable attempt. */
    bool restored;      /**< Original layout and command state were restored. */
};
/** @brief Explicit rollback outcome, including recovery of the applied layout. */
struct pw_rollback_outcome {
    bool attempted;     /**< Preflight passed and restoration was attempted. */
    int fallback_error; /**< Applied-layout recovery error after a failed rollback. */
    int disable_error;  /**< Final containment error if neither layout was recovered. */
};

/**
 * @brief Require a successful configuration write and matching readback.
 * @param write_word Backend writing one configuration word.
 * @param read_word Backend reading that word.
 * @param opaque Backend-specific device or mock state.
 * @param expected Word value required before proceeding.
 * @return Zero only on matching readback; -EIO on any write/read/mismatch failure.
 */
static inline int pw_verified_word(int (*write_word)(void *, pw_u16),
                                   int (*read_word)(void *, pw_u16 *), void *opaque,
                                   pw_u16 expected)
{
    pw_u16 actual;
    if (write_word(opaque, expected) || read_word(opaque, &actual) || actual != expected)
        return -EIO;
    return 0;
}
/**
 * @brief Disable new DMA and memory decoding before draining pending requests.
 * @param ops Backend operations.
 * @param opaque Backend state.
 * @return Zero after disable and drain; first backend error otherwise.
 */
static inline int pw_quiesce(const struct pw_operations *ops, void *opaque)
{
    int ret = ops->commands(opaque, true);
    return ret ? ret : ops->drain(opaque);
}
/**
 * @brief Reconstruct a saved layout and contain any failed recovery.
 * @param ops Backend operations.
 * @param opaque Backend state.
 * @param original True for the original layout; false for the previously applied layout.
 * @param disable_error Output of the final disable attempt on failure, zero on success.
 * @return Zero after full recovery; first operation error otherwise.
 */
static inline int pw_recover(const struct pw_operations *ops, void *opaque, bool original,
                             int *disable_error)
{
    int ret;
    *disable_error = 0;
    if ((ret = pw_quiesce(ops, opaque)) || (ret = ops->release(opaque)) ||
        (ret = ops->register_layout(opaque, original)) || (ret = ops->program(opaque, original)) ||
        (ret = ops->commands(opaque, false)) || (ret = ops->save(opaque))) {
        *disable_error = ops->commands(opaque, true);
        return ret;
    }
    return 0;
}
/**
 * @brief Apply a plan atomically with compensation for every failing stage.
 * @param ops Backend operations.
 * @param opaque Backend state.
 * @param fail_step Zero normally; one fails after release, two after programming.
 * @param outcome Recovery/containment details, initialized by this helper.
 * @return Zero after success, otherwise the operation or failed-recovery error.
 */
static inline int pw_apply(const struct pw_operations *ops, void *opaque, unsigned int fail_step,
                           struct pw_outcome *outcome)
{
    int ret;
    *outcome = (struct pw_outcome){0};
    ret = pw_quiesce(ops, opaque);
    if (ret) {
        /* The layout is untouched: only commands need restoration. */
        outcome->recovery_error = ops->commands(opaque, false);
        outcome->restored = !outcome->recovery_error;
        if (outcome->recovery_error) {
            outcome->containment = true;
            outcome->disable_error = ops->commands(opaque, true);
        }
        return ret;
    }
    if ((ret = ops->release(opaque)))
        goto recover;
    if (fail_step == 1) {
        ret = -ECANCELED;
        goto recover;
    }
    if ((ret = ops->register_layout(opaque, false)) || (ret = ops->program(opaque, false)))
        goto recover;
    if (fail_step == 2) {
        ret = -ECANCELED;
        goto recover;
    }
    if ((ret = ops->commands(opaque, false)) || (ret = ops->save(opaque)))
        goto recover;
    return 0;
recover:
    outcome->recovery_error = pw_recover(ops, opaque, true, &outcome->disable_error);
    outcome->restored = !outcome->recovery_error;
    outcome->containment = !!outcome->recovery_error;
    return outcome->recovery_error ? outcome->recovery_error : ret;
}
/**
 * @brief Restore original allocations only after checking their availability.
 * @param ops Backend operations.
 * @param opaque Backend state.
 * @param preflight Non-mutating validation of current ownership and old destination ranges.
 * @param outcome Restoration/fallback details, initialized here.
 * @return Zero after original-layout restoration; otherwise the preflight/recovery error.
 * A failed reconstruction attempts to recover the previously applied layout.
 */
static inline int pw_rollback(const struct pw_operations *ops, void *opaque,
                              int (*preflight)(void *), struct pw_rollback_outcome *outcome)
{
    int ret;
    *outcome = (struct pw_rollback_outcome){0};
    ret = preflight(opaque);
    if (ret)
        return ret;
    outcome->attempted = true;
    ret = pw_recover(ops, opaque, true, &outcome->disable_error);
    if (ret)
        outcome->fallback_error = pw_recover(ops, opaque, false, &outcome->disable_error);
    return ret;
}
#endif
