/* SPDX-License-Identifier: GPL-2.0-only */
/** @file test_transaction.c
 * @brief Regression tests using the production transaction engine with mocked PCI operations.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../plan.h"
#include "../transaction.h"

/** @brief Small PCI/resource model with event logging and one-shot failures. */
struct model {
    char log[256];                     /**< Ordered backend operations. */
    unsigned int length;               /**< Number of logged operations. */
    unsigned int calls[256];           /**< Per-event invocation counts. */
    char fail;                         /**< Event to fail, or zero. */
    unsigned int occurrence;           /**< Invocation to fail. */
    pw_u16 command;                    /**< Emulated PCI command register. */
    unsigned int ignore_disable_after; /**< Ignore disables from this invocation onward. */
    bool ignore_write;                 /**< Successful writes may deliberately do nothing. */
    bool drained;                      /**< Transactions have drained after mastering disable. */
    bool partial_failure;              /**< A failing operation has already changed some objects. */
    int resources;                     /**< Zero: original; one: new; minus one: released. */
    int hardware;                      /**< Zero: original; one: new. */
    bool old_occupied;                 /**< Another allocation now occupies the old range. */
};

/** @brief Record an operation and inject a configured one-shot error.
 * @param m Mock state. @param event Operation code.
 * @return Zero normally, -EIO at the selected failure.
 */
static int event(struct model *m, char event)
{
    unsigned char index = (unsigned char)event;
    assert(m->length + 1 < sizeof(m->log));
    m->log[m->length++] = event;
    m->log[m->length] = '\0';
    m->calls[index]++;
    return m->fail == event && m->calls[index] == m->occurrence ? -EIO : 0;
}
/** @brief Emulate a write that can acknowledge without changing hardware.
 * @param opaque Mock state. @param value Requested command word.
 * @return Zero, including when the emulated device ignores the write.
 */
static int write_word(void *opaque, pw_u16 value)
{
    struct model *m = opaque;
    if (!m->ignore_write &&
        !(value == 0 && m->ignore_disable_after && m->calls['D'] >= m->ignore_disable_after))
        m->command = value;
    return 0;
}
/** @brief Read the actual emulated command register.
 * @param opaque Mock state. @param value Output register value.
 * @return Zero.
 */
static int read_word(void *opaque, pw_u16 *value)
{
    *value = ((struct model *)opaque)->command;
    return 0;
}
/** @brief Use production readback verification for mock command writes.
 * @param opaque Mock state. @param disable Select disable versus original commands.
 * @return First injected or readback error, or zero.
 */
static int commands(void *opaque, bool disable)
{
    struct model *m = opaque;
    int ret = event(m, disable ? 'D' : 'E');
    if (ret) {
        if (!disable)
            m->command = 6; /* Simulate partial successful re-enable. */
        return ret;
    }
    if (disable)
        m->drained = false;
    return pw_verified_word(write_word, read_word, m, disable ? 0 : 6);
}
/** @brief Require mastering off before draining requests.
 * @param opaque Mock state. @return Injected failure or zero.
 */
static int drain(void *opaque)
{
    struct model *m = opaque;
    assert(m->command == 0);
    int ret = event(m, 'Q');
    m->drained = !ret;
    return ret;
}
/** @brief Require successful quiescence before releasing resources.
 * @param opaque Mock state. @return Injected failure or zero.
 */
static int release(void *opaque)
{
    struct model *m = opaque;
    assert(m->command == 0 && m->drained);
    int ret = event(m, 'L');
    if (!ret)
        m->resources = -1;
    return ret;
}
/** @brief Model registration of original or planned resources.
 * @param opaque Mock state. @param original Select original layout.
 * @return Injected failure or zero.
 */
static int register_layout(void *opaque, bool original)
{
    struct model *m = opaque;
    int ret = event(m, original ? 'O' : 'N');
    if (!ret)
        m->resources = original ? 0 : 1;
    else if (m->partial_failure)
        m->resources = 2;
    return ret;
}
/** @brief Model verified hardware programming with matching resources.
 * @param opaque Mock state. @param original Select original layout.
 * @return Injected failure or zero.
 */
static int program(void *opaque, bool original)
{
    struct model *m = opaque;
    assert(m->command == 0 && m->resources == (original ? 0 : 1));
    int ret = event(m, original ? 'R' : 'P');
    if (!ret)
        m->hardware = original ? 0 : 1;
    else if (m->partial_failure)
        m->hardware = 2;
    return ret;
}
/** @brief Verify enabled state when refreshing PCI power-management state.
 * @param opaque Mock state. @return Injected failure or zero.
 */
static int save(void *opaque)
{
    struct model *m = opaque;
    assert(m->command == 6 && m->resources == m->hardware);
    return event(m, 'S');
}
/** @brief Mock backend used by the production transaction routines. */
static const struct pw_operations operations = {commands,        drain,   release,
                                                register_layout, program, save};
/** @brief Reject an occupied original range before touching current hardware.
 * @param opaque Mock state. @return Zero if available, -EBUSY for a conflict.
 */
static int preflight(void *opaque)
{
    struct model *m = opaque;
    struct pw_interval used = {256, 511};
    return pw_original_range_free(256, 511, 0, 1023, &used, m->old_occupied ? 1 : 0) ? -EBUSY : 0;
}
/** @brief Exercise a successful write that fails to change the command register. */
static void verified_command(void)
{
    struct model m = {.command = 6, .ignore_write = true};
    struct pw_outcome result;
    assert(pw_apply(&operations, &m, 0, &result) == -EIO);
    assert(!strcmp(m.log, "DE"));
    assert(m.resources == 0 && m.hardware == 0 && result.restored);
    assert(!result.containment);
}
/** @brief Require disable then drain before any allocation update. */
static void dma_ordering(void)
{
    struct model m = {.command = 6};
    struct pw_outcome result;
    assert(!pw_apply(&operations, &m, 0, &result));
    assert(!strcmp(m.log, "DQLNPES"));
    assert(m.command == 6 && m.resources == 1 && m.hardware == 1);
}
/** @brief Inject failures at each apply stage and after both explicit injection points. */
static void apply_failures(void)
{
    const char failures[] = "DQLNPES";
    for (unsigned int i = 0; i < sizeof(failures) - 1; i++) {
        struct model m = {.command = 6, .fail = failures[i], .occurrence = 1};
        struct pw_outcome result;
        assert(pw_apply(&operations, &m, 0, &result) == -EIO);
        assert(result.restored && !result.recovery_error);
        assert(m.command == 6 && m.resources == 0 && m.hardware == 0);
    }
    /* Registration/programming may fail after earlier resources already changed. */
    const char partial_events[] = "NP";
    for (unsigned int i = 0; i < sizeof(partial_events) - 1; i++) {
        struct model m = {
            .command = 6, .fail = partial_events[i], .occurrence = 1, .partial_failure = true};
        struct pw_outcome result;
        assert(pw_apply(&operations, &m, 0, &result) == -EIO);
        assert(result.restored && m.command == 6 && m.resources == 0 && m.hardware == 0);
    }
    for (unsigned int step = 1; step <= 2; step++) {
        struct model m = {.command = 6};
        struct pw_outcome result;
        assert(pw_apply(&operations, &m, step, &result) == -ECANCELED);
        assert(result.restored && m.command == 6 && m.resources == 0 && m.hardware == 0);
    }
}
/** @brief A failed restore-enable operation must attempt a verified disable again. */
static void rollback_failure(void)
{
    struct model m = {.command = 6, .fail = 'E', .occurrence = 1};
    struct pw_outcome result;
    assert(pw_apply(&operations, &m, 2, &result) == -EIO);
    assert(result.containment && !result.disable_error && !result.restored);
    assert(m.log[m.length - 1] == 'D' && m.command == 0);
    assert(m.resources == 0 && m.hardware == 0);
    m = (struct model){.command = 6, .fail = 'E', .occurrence = 1, .ignore_disable_after = 3};
    assert(pw_apply(&operations, &m, 2, &result) == -EIO);
    assert(result.containment && result.disable_error == -EIO && !result.restored);
    assert(m.command == 6); /* The engine must report disable as unverified. */
}
/** @brief Reject a newly occupied original range without dismantling the active layout. */
static void rollback_destination(void)
{
    struct model m = {.command = 6, .resources = 1, .hardware = 1, .old_occupied = true};
    struct pw_rollback_outcome result;
    assert(pw_rollback(&operations, &m, preflight, &result) == -EBUSY);
    assert(!result.attempted && m.length == 0 && m.command == 6);
    assert(m.resources == 1 && m.hardware == 1);
}
/** @brief Recover the applied layout when an explicit restoration hits a resource conflict. */
static void rollback_fallback(void)
{
    struct model m = {.command = 6, .resources = 1, .hardware = 1, .fail = 'O', .occurrence = 1};
    struct pw_rollback_outcome result;
    assert(pw_rollback(&operations, &m, preflight, &result) == -EIO);
    assert(result.attempted && !result.fallback_error);
    assert(m.command == 6 && m.resources == 1 && m.hardware == 1);
    assert(strstr(m.log, "DQLODDQLNPES") != NULL);
}
/** @brief Dispatch one transaction regression case.
 * @param argc Argument count. @param argv Program and case name.
 * @return Zero for success, two for an unknown case; assertions abort on failure.
 */
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "verified_command"))
        verified_command();
    else if (!strcmp(argv[1], "dma_ordering"))
        dma_ordering();
    else if (!strcmp(argv[1], "apply_failures"))
        apply_failures();
    else if (!strcmp(argv[1], "rollback_failure"))
        rollback_failure();
    else if (!strcmp(argv[1], "rollback_destination"))
        rollback_destination();
    else if (!strcmp(argv[1], "rollback_fallback"))
        rollback_fallback();
    else
        return 2;
    puts("PASS");
    return 0;
}
