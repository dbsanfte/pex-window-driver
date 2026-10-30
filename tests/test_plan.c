/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../plan.h"

static void five_gpus(void)
{
    pw_u64 root = 0x120000000000ULL, limit = root + (512 * PW_GIB) - 1;
    pw_u64 a, b, bar, end, aux, auxend;
    for (unsigned int i = 0; i < 5; ++i) {
        assert(!pw_place(root + i * PW_SLOT_BYTES, limit, PW_SLOT_BYTES,
                         PW_SLOT_BYTES, &a, &b));
        assert(!pw_place(a, b, 32 * PW_GIB, 32 * PW_GIB, &bar, &end));
        /* NVIDIA's 32 MiB auxiliary BAR is larger than AMD's 2 MiB BAR. */
        assert(!pw_place(end + 1, b, 32ULL << 20, 32ULL << 20, &aux, &auxend));
        assert(!pw_overlap(bar, end, aux, auxend));
        assert(auxend <= b && b <= limit);
        assert(bar % (32 * PW_GIB) == 0);
    }
    /* Five independently aligned AMD windows cannot fit in 256 GiB. */
    pw_u64 cursor = 0;
    for (unsigned int i = 0; i < 5; ++i) {
        assert(!pw_place(cursor, UINT64_MAX - 1, 32 * PW_GIB + (2ULL << 20),
                         32 * PW_GIB, &a, &b));
        cursor = b + 1;
    }
    assert(cursor == 288 * PW_GIB + (2ULL << 20));
}
static void nested_branches(void)
{
    const unsigned int slots[] = {1, 2, 2};
    pw_u64 cursor = 0, a, b, bytes, last = 0;
    for (unsigned int i = 0; i < 3; ++i) {
        assert(!pw_slot_span(slots[i], PW_SLOT_BYTES, &bytes));
        assert(!pw_place(cursor, (512 * PW_GIB) - 1, bytes, PW_SLOT_BYTES, &a, &b));
        assert(i == 0 || last < a);
        for (unsigned int j = 0; j < slots[i]; ++j) {
            pw_u64 child, end;
            assert(!pw_place(a + j * PW_SLOT_BYTES, b, PW_SLOT_BYTES,
                             PW_SLOT_BYTES, &child, &end));
            assert(child >= a && end <= b);
        }
        cursor = b + 1;
        last = b;
    }
    assert(cursor == 320 * PW_GIB);
}
static void boundaries(void)
{
    pw_u64 a, b;
    assert(pw_place(0, 1023, 0, 1, &a, &b));
    assert(pw_place(0, 1023, 1, 0, &a, &b));
    assert(pw_place(0, 1023, 1, 3, &a, &b));
    assert(pw_place(1024, 1023, 1, 1, &a, &b));
    assert(pw_place(UINT64_MAX, UINT64_MAX, 1, 2, &a, &b));
    assert(pw_place(UINT64_MAX - 7, UINT64_MAX, 9, 1, &a, &b));
    assert(!pw_place(UINT64_MAX - 7, UINT64_MAX, 8, 8, &a, &b));
    assert(a == UINT64_MAX - 7 && b == UINT64_MAX);
    assert(!pw_place(1, 8191, 4096, 4096, &a, &b));
    assert(a == 4096 && b == 8191);
    assert(pw_place(1, 8190, 4096, 4096, &a, &b));
}
static void conflicts(void)
{
    assert(pw_overlap(0, 10, 10, 20));
    assert(!pw_overlap(0, 9, 10, 20));
    assert(pw_overlap(0, 100, 20, 30));
    assert(pw_overlap(20, 30, 0, 100));
    /* A host reservation in the first half requires using the second half. */
    pw_u64 a, b;
    assert(!pw_place(0, 1024 * PW_GIB - 1, (512 * PW_GIB), (512 * PW_GIB), &a, &b));
    assert(pw_overlap(a, b, 64 * PW_GIB, 65 * PW_GIB - 1));
    assert(!pw_place(65 * PW_GIB, 1024 * PW_GIB - 1, (512 * PW_GIB),
                     (512 * PW_GIB), &a, &b));
    assert(a == 512 * PW_GIB && b == 1024 * PW_GIB - 1);
    assert(!pw_overlap(a, b, 64 * PW_GIB, 65 * PW_GIB - 1));
}
static void capacity(void)
{
    pw_u64 bytes, a, b;
    assert(pw_slot_span(0, PW_SLOT_BYTES, &bytes));
    assert(!pw_slot_span(6, PW_SLOT_BYTES, &bytes) && bytes == 384 * PW_GIB);
    assert(pw_slot_span(UINT64_MAX / PW_SLOT_BYTES + 1, PW_SLOT_BYTES, &bytes));
    assert(pw_slot_span(1, 0, &bytes));
    assert(!pw_slot_span(5, PW_SLOT_BYTES, &bytes) && bytes == 320 * PW_GIB);
    assert(pw_place(0, 256 * PW_GIB - 1, bytes, PW_SLOT_BYTES, &a, &b));
    assert(!pw_place(0, 512 * PW_GIB - 1, bytes, PW_SLOT_BYTES, &a, &b));
}
static void many_switches(void)
{
    /* Hundreds of independently reserved PEX trees, no 8-tree/5-GPU ceiling. */
    struct pw_interval used[1024];
    pw_u64 base = 1ULL << 42, span, limit = (1ULL << 49) - 1;
    unsigned int n = 0;
    for (; n < 1024; ++n) {
        pw_u64 slots = 1 + n % 13;
        assert(!pw_slot_span(slots, PW_SLOT_BYTES, &span));
        if (pw_find_space(base, limit, span, PW_SLOT_BYTES, used, n,
                          &used[n].start, &used[n].end))
            break;
        assert(used[n].start >= base && used[n].end <= limit);
        assert(used[n].end - used[n].start + 1 == span);
        for (unsigned int j = 0; j < n; ++j)
            assert(!pw_overlap(used[n].start, used[n].end, used[j].start, used[j].end));
    }
    assert(n == 1024);
}
static void shared_aperture(void)
{
    struct pw_interval used[5];
    pw_u64 a, b;
    /* Three five-GPU trees fit in 1 TiB. A fourth must fail atomically at plan time. */
    for (unsigned int i = 0; i < 3; ++i)
        assert(!pw_find_space(0, 1024 * PW_GIB - 1, 320 * PW_GIB, PW_SLOT_BYTES,
                              used, i, &used[i].start, &used[i].end));
    assert(pw_find_space(0, 1024 * PW_GIB - 1, 320 * PW_GIB, PW_SLOT_BYTES, used, 3, &a, &b));
    /* Exact exhaustion: the final 64 GiB slot is usable, one byte more is not. */
    assert(!pw_find_space(0, 1024 * PW_GIB - 1, PW_SLOT_BYTES, PW_SLOT_BYTES, used, 3, &a, &b));
    assert(a == 960 * PW_GIB && b == 1024 * PW_GIB - 1);
    used[3] = (struct pw_interval){a, b};
    assert(pw_find_space(0, 1024 * PW_GIB - 1, 1, 1, used, 4, &a, &b));
    /* A disjoint host aperture has its own capacity. */
    assert(!pw_find_space(2ULL << 40, (3ULL << 40) - 1, 320 * PW_GIB,
                          PW_SLOT_BYTES, used, 4, &a, &b));
}
static void fragmentation(void)
{
    /* Unsorted firmware reservations, one crossing the lower aperture boundary. */
    struct pw_interval used[] = {{700, 799}, {0, 149}, {256, 511}, {900, UINT64_MAX}};
    pw_u64 a, b;
    assert(!pw_find_space(128, 1023, 128, 64, used, 4, &a, &b));
    assert(a == 512 && b == 639);
    assert(pw_find_space(128, 1023, 256, 64, used, 4, &a, &b));
    assert(pw_find_space(UINT64_MAX - 7, UINT64_MAX, 1, 1, used, 4, &a, &b));
    assert(!pw_find_space(UINT64_MAX - 7, UINT64_MAX, 8, 8, NULL, 0, &a, &b));
    assert(b == UINT64_MAX);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "five_gpus")) five_gpus();
    else if (!strcmp(argv[1], "nested_branches")) nested_branches();
    else if (!strcmp(argv[1], "boundaries")) boundaries();
    else if (!strcmp(argv[1], "conflicts")) conflicts();
    else if (!strcmp(argv[1], "capacity")) capacity();
    else if (!strcmp(argv[1], "many_switches")) many_switches();
    else if (!strcmp(argv[1], "shared_aperture")) shared_aperture();
    else if (!strcmp(argv[1], "fragmentation")) fragmentation();
    else return 2;
    puts("PASS");
    return 0;
}
