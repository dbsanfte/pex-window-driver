// SPDX-License-Identifier: GPL-2.0-only
/* Transactional, pre-driver MMIO allocation for discovered PEX GPU trees.
 * This is an allocation helper, not a replacement for the pcieport driver.
 * All changed resource objects belong to pci_dev, and survive module unload.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include "plan.h"

#define PEX_VENDOR 0x1000
#define PEX_DEVICE 0xc010
#define PREF_RES PCI_BRIDGE_PREF_MEM_WINDOW

static bool apply;
module_param(apply, bool, 0444);
MODULE_PARM_DESC(apply, "Apply allocation (default: read-only plan)");
static unsigned int fail_step;
module_param(fail_step, uint, 0444);
MODULE_PARM_DESC(fail_step, "Testing only: inject failure at transaction step 1 or 2");
static unsigned int bar_gib = 32, slot_gib = 64, empty_min_width = 8;
module_param(bar_gib, uint, 0444);
module_param(slot_gib, uint, 0444);
module_param(empty_min_width, uint, 0444);
MODULE_PARM_DESC(bar_gib, "Requested VRAM BAR GiB (power of two, default 32)");
MODULE_PARM_DESC(slot_gib, "Address space per GPU/empty slot GiB (power of two, default 64)");
MODULE_PARM_DESC(empty_min_width, "Reserve empty PEX downstream ports at least this wide (0 disables, default 8)");
static char *roots;
module_param(roots, charp, 0444);
MODULE_PARM_DESC(roots, "Optional comma-separated root port BDFs; default discovers all PEX trees");

struct node {
    struct pci_dev *dev;
    int parent, tree, vrambar, rebar;
    u64 slots, start, end, cursor, vram_bytes;
    bool powered, locked;
    u16 command;
    u32 rebar_old, rebar_new;
    u32 pref[3];
};
struct mapping {
    struct resource *res, *old_parent, *new_parent;
    const char *old_name;
    resource_size_t old_start, old_end, start, end;
    unsigned long old_flags, flags;
    int node, bar;
    u32 low, high;
};
struct tree {
    int root, last;
    struct resource *host;
    resource_size_t start, end, bytes;
};
struct context {
    struct node *nodes;
    struct mapping *maps;
    struct tree *trees;
    int nn, nm, nt, capacity;
    bool changed;
};
static struct context *ctx;
static DEFINE_MUTEX(operation_lock);

static bool pex(struct pci_dev *d)
{
    return (d->vendor == PEX_VENDOR && d->device == PEX_DEVICE) ||
           (d->vendor == 0x10b5 && pci_is_pcie(d));
}
static bool graphics(struct pci_dev *d)
{
    return (d->class >> 16) == PCI_BASE_CLASS_DISPLAY;
}
static bool pref64(struct resource *r)
{
    return (r->flags & (IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_MEM_64)) ==
           (IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_MEM_64);
}
static int cfg_word(struct pci_dev *d, int pos, u16 *v)
{
    return pci_read_config_word(d, pos, v) ? -EIO : 0;
}
static int cfg_dword(struct pci_dev *d, int pos, u32 *v)
{
    return pci_read_config_dword(d, pos, v) ? -EIO : 0;
}

static int gather_one(struct pci_dev *d, int parent, int ti)
{
    struct node *n;
    u32 link;
    int idx, ret;
    if (ctx->nn == ctx->capacity)
        return -EOVERFLOW; /* enumeration changed despite rescan/remove lock */
    idx = ctx->nn++;
    n = &ctx->nodes[idx];
    n->dev = pci_dev_get(d);
    n->parent = parent;
    n->tree = ti;
    n->vrambar = -1;
    ret = pm_runtime_resume_and_get(&d->dev);
    if (ret < 0)
        return ret;
    n->powered = true;
    if (cfg_word(d, PCI_COMMAND, &n->command))
        return -EIO;
    if (pci_is_bridge(d)) {
        if (!pci_is_pcie(d) || !d->subordinate ||
            cfg_dword(d, PCI_PREF_MEMORY_BASE, &n->pref[0]) ||
            cfg_dword(d, PCI_PREF_BASE_UPPER32, &n->pref[1]) ||
            cfg_dword(d, PCI_PREF_LIMIT_UPPER32, &n->pref[2]))
            return -EIO;
        if (pex(d) && pci_pcie_type(d) == PCI_EXP_TYPE_DOWNSTREAM &&
            list_empty(&d->subordinate->devices) && empty_min_width) {
            if (pcie_capability_read_dword(d, PCI_EXP_LNKCAP, &link))
                return -EIO;
            if (((link & PCI_EXP_LNKCAP_MLW) >> 4) >= empty_min_width)
                n->slots = 1;
        }
    } else if (graphics(d)) {
        if (d->is_virtfn || (d->vendor != 0x1002 && d->vendor != 0x10de)) {
            pci_err(d, "pex_window: unsupported GPU vendor or virtual function\n");
            return -ENODEV;
        }
        n->vrambar = d->vendor == 0x10de ? 1 : 0;
        n->slots = 1;
    } else if (!((d->class >> 8) == PCI_CLASS_MULTIMEDIA_HD_AUDIO &&
                 (d->vendor == 0x1002 || d->vendor == 0x10de)) &&
               !(pex(d) && (d->class >> 8) == PCI_CLASS_STORAGE_OTHER)) {
        pci_err(d, "pex_window: foreign endpoint in target tree; use roots= to restrict discovery\n");
        return -ENODEV;
    }
    return idx;
}

static int find_pex(struct pci_dev *d, void *data)
{
    bool *found = data;
    if (pex(d) && pci_is_bridge(d))
        *found = true;
    return 0;
}

static bool selected_root(struct pci_dev *d)
{
    const char *p = roots, *end;
    size_t length = strlen(pci_name(d));
    if (!p || !*p)
        return true;
    do {
        end = strchrnul(p, ',');
        if (end - p == length && !strncmp(p, pci_name(d), length))
            return true;
        p = *end ? end + 1 : end;
    } while (*p);
    return false;
}

static int discover(void)
{
    struct pci_dev *d = NULL, *child;
    int ret, i, count = 0;
    for_each_pci_dev(d) {
        if (count == INT_MAX / PCI_STD_NUM_BARS) {
            pci_dev_put(d);
            return -EOVERFLOW;
        }
        count++;
    }
    ctx->capacity = count;
    ctx->nodes = kvcalloc(count, sizeof(*ctx->nodes), GFP_KERNEL);
    ctx->maps = kvcalloc(count, PCI_STD_NUM_BARS * sizeof(*ctx->maps), GFP_KERNEL);
    ctx->trees = kvcalloc(count, sizeof(*ctx->trees), GFP_KERNEL);
    if (!ctx->nodes || !ctx->maps || !ctx->trees)
        return -ENOMEM;
    for_each_pci_dev(d) {
        struct tree *t;
        bool found = false;
        if (!pci_is_root_bus(d->bus) || !pci_is_bridge(d) || !d->subordinate ||
            pci_pcie_type(d) != PCI_EXP_TYPE_ROOT_PORT || !selected_root(d))
            continue;
        pci_walk_bus(d->subordinate, find_pex, &found);
        if (!found)
            continue;
        t = &ctx->trees[ctx->nt];
        ret = gather_one(d, -1, ctx->nt);
        if (ret < 0)
            goto fail;
        t->root = ret;
        /* Breadth first, avoiding recursion even for deeply cascaded switches. */
        for (i = t->root; i < ctx->nn; i++) {
            struct pci_bus *bus = ctx->nodes[i].dev->subordinate;
            if (!bus)
                continue;
            list_for_each_entry(child, &bus->devices, bus_list) {
                ret = gather_one(child, i, ctx->nt);
                if (ret < 0)
                    goto fail;
            }
        }
        t->last = ctx->nn;
        for (i = t->last - 1; i > t->root; i--) {
            struct node *n = &ctx->nodes[i];
            if (ctx->nodes[n->parent].slots > U64_MAX - n->slots) {
                ret = -EOVERFLOW;
                goto fail;
            }
            ctx->nodes[n->parent].slots += n->slots;
        }
        if (pw_slot_span(ctx->nodes[t->root].slots, (u64)slot_gib * PW_GIB, &t->bytes) ||
            !pref64(&d->resource[PREF_RES]) || !d->resource[PREF_RES].parent) {
            pci_err(d, "pex_window: no GPU capacity or assigned 64-bit root aperture\n");
            ret = -ENODEV;
            goto fail;
        }
        t->host = d->resource[PREF_RES].parent;
        if (!(t->host->flags & IORESOURCE_MEM) || t->host->start < (1ULL << 32)) {
            ret = -ENOSPC;
            goto fail;
        }
        pci_info(d, "pex_window: discovered %llu slots, requires %llu GiB\n",
                 ctx->nodes[t->root].slots, t->bytes / PW_GIB);
        ctx->nt++;
    }
    return ctx->nt ? 0 : -ENODEV;
fail:
    pci_dev_put(d);
    return ret;
}

static int rebar_plan(struct node *n)
{
    struct pci_dev *d = n->dev;
    struct resource *r = &d->resource[n->vrambar];
    int pos, count, i, size = ilog2(bar_gib) + 10;
    u32 ctrl, cap;
    n->vram_bytes = max_t(u64, resource_size(r), (u64)bar_gib * PW_GIB);
    if (!pref64(r) || n->vram_bytes > (u64)slot_gib * PW_GIB)
        return -EINVAL;
    if (resource_size(r) == n->vram_bytes)
        return 0;
    pos = pci_find_ext_capability(d, PCI_EXT_CAP_ID_REBAR);
    if (!pos || cfg_dword(d, pos + PCI_REBAR_CTRL, &ctrl))
        return -ENOTSUPP;
    count = (ctrl & PCI_REBAR_CTRL_NBAR_MASK) >> PCI_REBAR_CTRL_NBAR_SHIFT;
    if (!count || count > PCI_STD_NUM_BARS)
        return -EINVAL;
    for (i = 0; i < count; i++, pos += 8) {
        if (cfg_dword(d, pos + PCI_REBAR_CTRL, &ctrl) ||
            cfg_dword(d, pos + PCI_REBAR_CAP, &cap))
            return -EIO;
        if ((ctrl & PCI_REBAR_CTRL_BAR_IDX) != n->vrambar)
            continue;
        if (!(cap & BIT(size + 4)))
            return -ENOTSUPP;
        n->rebar = pos + PCI_REBAR_CTRL;
        n->rebar_old = ctrl;
        n->rebar_new = (ctrl & ~PCI_REBAR_CTRL_BAR_SIZE) | (size << PCI_REBAR_CTRL_BAR_SHIFT);
        return 0;
    }
    return -ENOTSUPP;
}

static int add_map(int ni, int bar, struct resource *parent,
                   resource_size_t start, resource_size_t end)
{
    struct mapping *m;
    struct pci_dev *d = ctx->nodes[ni].dev;
    struct resource *r = &d->resource[bar];
    if (ctx->nm == ctx->capacity * PCI_STD_NUM_BARS || start > end)
        return -E2BIG;
    m = &ctx->maps[ctx->nm++];
    m->node = ni;
    m->bar = bar;
    m->res = r;
    m->old_parent = r->parent;
    m->old_start = r->start;
    m->old_end = r->end;
    m->old_flags = r->flags;
    m->old_name = r->name;
    m->new_parent = parent;
    m->start = start;
    m->end = end;
    m->flags = bar == PREF_RES ? (IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_MEM_64 | 1) :
               (r->flags & ~(IORESOURCE_UNSET | IORESOURCE_STARTALIGN));
    if (bar < PCI_STD_NUM_BARS) {
        u64 old_address;
        if (!pref64(r) || (apply && r->child) || !r->parent ||
            cfg_dword(d, PCI_BASE_ADDRESS_0 + 4 * bar, &m->low) ||
            cfg_dword(d, PCI_BASE_ADDRESS_0 + 4 * (bar + 1), &m->high))
            return -EINVAL;
        old_address = ((u64)m->high << 32) | (m->low & PCI_BASE_ADDRESS_MEM_MASK);
        if (old_address != r->start) {
            pci_err(d, "pex_window: non-identity or stale BAR mapping\n");
            return -EINVAL;
        }
    }
    pci_info(d, "pex_window: plan resource %d [%#llx-%#llx] (%llu MiB)\n",
             bar, (u64)start, (u64)end, (u64)(end - start + 1) >> 20);
    return 0;
}

static int plan_tree(struct tree *t)
{
    int ni, i, ret;
    u64 slot_bytes = (u64)slot_gib * PW_GIB;
    struct node *root = &ctx->nodes[t->root];
    root->start = t->start;
    root->end = t->end;
    for (ni = t->root; ni < t->last; ni++) {
        struct node *n = &ctx->nodes[ni], *parent;
        struct pci_dev *d = n->dev;
        struct resource *pr;
        u64 a, b, bytes;
        if (!n->slots)
            continue;
        if (n->parent < 0) {
            pr = t->host;
        } else {
            parent = &ctx->nodes[n->parent];
            pr = &parent->dev->resource[PREF_RES];
            if (pw_slot_span(n->slots, slot_bytes, &bytes) ||
                pw_place(parent->cursor, parent->end, bytes, slot_bytes, &n->start, &n->end) ||
                n->end == U64_MAX)
                return -ENOSPC;
            parent->cursor = n->end + 1;
        }
        n->cursor = n->start;
        if (pci_is_bridge(d)) {
            if (d->subordinate->resource[2] != &d->resource[PREF_RES] ||
                (n->pref[0] & 0x000f000f) != 0x00010001)
                return -ENOTSUPP;
            for (i = 0; i < PCI_STD_NUM_BARS; i++)
                if (d->resource[i].flags & IORESOURCE_PREFETCH)
                    return -ENOTSUPP;
            ret = add_map(ni, PREF_RES, pr, n->start, n->end);
            if (ret)
                return ret;
        } else if (n->vrambar >= 0) {
            ret = rebar_plan(n);
            if (ret)
                return ret;
            for (i = -1; i < PCI_STD_NUM_BARS; i++) {
                int bar = i < 0 ? n->vrambar : i;
                struct resource *r = &d->resource[bar];
                if ((i >= 0 && bar == n->vrambar) || !(r->flags & IORESOURCE_PREFETCH))
                    continue;
                if (!pref64(r))
                    return -ENOTSUPP;
                bytes = bar == n->vrambar ? n->vram_bytes : resource_size(r);
                if (pw_place(n->cursor, n->end, bytes, bytes, &a, &b) || b == U64_MAX)
                    return -ENOSPC;
                ret = add_map(ni, bar, pr, a, b);
                if (ret)
                    return ret;
                n->cursor = b + 1;
            }
        }
    }
    return 0;
}

static bool moving_root(struct resource *r)
{
    int i;
    for (i = 0; i < ctx->nt; i++)
        if (r == &ctx->nodes[ctx->trees[i].root].dev->resource[PREF_RES])
            return true;
    return false;
}

static int make_plan(void)
{
    int i, j, ret;
    for (i = 0; i < ctx->nt; i++) {
        struct tree *t = &ctx->trees[i];
        struct resource *other;
        struct pw_interval *used;
        unsigned int count = 0, k = 0;
        for (other = t->host->child; other; other = other->sibling)
            if (!moving_root(other))
                count++;
        used = kvcalloc((size_t)count + ctx->nt, sizeof(*used), GFP_KERNEL);
        if (!used)
            return -ENOMEM;
        for (other = t->host->child; other; other = other->sibling)
            if (!moving_root(other))
                used[k++] = (struct pw_interval){other->start, other->end};
        for (j = 0; j < i; j++)
            if (ctx->trees[j].host == t->host)
                used[k++] = (struct pw_interval){ctx->trees[j].start, ctx->trees[j].end};
        ret = pw_find_space(t->host->start, t->host->end, t->bytes,
                            (u64)slot_gib * PW_GIB, used, k, &t->start, &t->end);
        kvfree(used);
        if (ret) {
            pci_err(ctx->nodes[t->root].dev,
                    "pex_window: no contiguous %llu GiB window in host aperture %pR\n",
                    t->bytes / PW_GIB, t->host);
            return -ENOSPC;
        }
        ret = plan_tree(t);
        if (ret)
            return ret;
    }
    /* Never orphan another allocation. Driver claims are allowed for dry runs. */
    for (i = 0; i < ctx->nm; i++) {
        struct resource *child;
        if (!apply && ctx->maps[i].bar < PCI_STD_NUM_BARS)
            continue;
        for (child = ctx->maps[i].res->child; child; child = child->sibling) {
            for (j = 0; j < ctx->nm; j++)
                if (ctx->maps[j].res == child)
                    break;
            if (j == ctx->nm)
                return -EBUSY;
        }
    }
    return 0;
}

static int lock_endpoints(void)
{
    int i;
    for (i = 0; i < ctx->nn; i++) {
        struct node *n = &ctx->nodes[i];
        if (pci_is_bridge(n->dev))
            continue;
        device_lock(&n->dev->dev);
        n->locked = true;
        if (n->dev->driver) {
            pci_err(n->dev, "pex_window: detach endpoint driver %s first\n", n->dev->driver->name);
            return -EBUSY;
        }
    }
    return 0;
}
static void unlock_endpoints(void)
{
    int i;
    for (i = ctx->nn - 1; i >= 0; i--)
        if (ctx->nodes[i].locked) {
            device_unlock(&ctx->nodes[i].dev->dev);
            ctx->nodes[i].locked = false;
        }
}
static void release_power(void)
{
    int i;
    for (i = ctx->nn - 1; i >= 0; i--) {
        struct node *n = &ctx->nodes[i];
        if (n->powered) {
            pm_runtime_put(&n->dev->dev);
            n->powered = false;
        }
    }
}
static int acquire_power(void)
{
    int i, ret;
    for (i = 0; i < ctx->nn; i++) {
        struct node *n = &ctx->nodes[i];
        ret = pm_runtime_resume_and_get(&n->dev->dev);
        if (ret < 0)
            return ret;
        n->powered = true;
        if (!pci_device_is_present(n->dev))
            return -ENODEV;
    }
    return 0;
}
static int commands(bool disable)
{
    int i, ret = 0;
    for (i = 0; i < ctx->nn; i++) {
        struct node *n = &ctx->nodes[i];
        u16 cmd = n->command;
        if (disable)
            cmd &= ~(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        if (pci_write_config_word(n->dev, PCI_COMMAND, cmd))
            ret = -EIO;
    }
    return ret;
}
static int release_maps(void)
{
    int i;
    for (i = ctx->nm - 1; i >= 0; i--)
        if (ctx->maps[i].res->parent && release_resource(ctx->maps[i].res))
            return -EBUSY;
    return 0;
}
static int register_maps(bool restore)
{
    int i;
    for (i = 0; i < ctx->nm; i++) {
        struct mapping *m = &ctx->maps[i];
        struct resource *r = m->res;
        struct resource *parent = restore ? m->old_parent : m->new_parent;
        r->start = restore ? m->old_start : m->start;
        r->end = restore ? m->old_end : m->end;
        r->flags = restore ? m->old_flags : m->flags;
        r->name = restore ? m->old_name : pci_name(ctx->nodes[m->node].dev);
        if (parent && request_resource(parent, r)) {
            pr_err("pex_window: resource registration failed for %s/%d\n", r->name, m->bar);
            return -ENOSPC;
        }
    }
    return 0;
}
static int program_config(bool restore)
{
    int i, error = 0;
    u32 readback;
    for (i = 0; i < ctx->nn; i++) {
        struct node *n = &ctx->nodes[i];
        u32 ctrl = restore ? n->rebar_old : n->rebar_new;
        if (!n->rebar)
            continue;
        if (pci_write_config_dword(n->dev, n->rebar, ctrl) ||
            cfg_dword(n->dev, n->rebar, &readback) || readback != ctrl)
            error = -EIO;
    }
    for (i = 0; i < ctx->nm; i++) {
        struct mapping *m = &ctx->maps[i];
        struct node *n = &ctx->nodes[m->node];
        struct pci_dev *d = n->dev;
        u32 regs[3], offsets[3];
        int count, j;
        if (m->bar == PREF_RES) {
            regs[0] = restore ? n->pref[1] : upper_32_bits(m->start);
            regs[1] = restore ? n->pref[2] : upper_32_bits(m->end);
            regs[2] = restore ? n->pref[0] :
                      (((m->start >> 16) & 0xfff0) | 1 |
                       ((m->end & 0xfff00000ULL) | 0x10000));
            offsets[0] = PCI_PREF_BASE_UPPER32;
            offsets[1] = PCI_PREF_LIMIT_UPPER32;
            offsets[2] = PCI_PREF_MEMORY_BASE;
            count = 3;
        } else {
            regs[0] = restore ? m->high : upper_32_bits(m->start);
            regs[1] = restore ? m->low : lower_32_bits(m->start) | (m->low & 0xf);
            offsets[0] = PCI_BASE_ADDRESS_0 + 4 * (m->bar + 1);
            offsets[1] = PCI_BASE_ADDRESS_0 + 4 * m->bar;
            count = 2;
        }
        for (j = 0; j < count; j++)
            if (pci_write_config_dword(d, offsets[j], regs[j]) ||
                cfg_dword(d, offsets[j], &readback) || readback != regs[j])
                error = -EIO;
    }
    return error;
}
static int save_states(void)
{
    int i, ret = 0;
    for (i = 0; i < ctx->nn; i++)
        if (pci_save_state(ctx->nodes[i].dev))
            ret = -EIO;
    return ret;
}
static int restore_all(void)
{
    int ret, error = 0;
    if (commands(true))
        error = -EIO;
    ret = release_maps();
    if (ret)
        return ret;
    ret = register_maps(true);
    if (ret)
        return ret;
    if (program_config(true))
        error = -EIO;
    /* Never restore decoding when configuration restoration did not verify. */
    if (error)
        return error;
    if (commands(false) || save_states())
        return -EIO;
    ctx->changed = false;
    pr_info("pex_window: baseline resource layout restored and verified\n");
    return 0;
}

static int perform(void)
{
    int ret, rr, i;
    for (i = 0; i < ctx->nn; i++) {
        struct pci_dev *d = ctx->nodes[i].dev;
        if (!pci_is_bridge(d) && !pci_wait_for_pending_transaction(d)) {
            ret = -EBUSY;
            goto out;
        }
    }
    ret = commands(true);
    if (ret)
        goto rollback;
    ret = release_maps();
    if (ret)
        goto rollback;
    if (fail_step == 1) {
        ret = -ECANCELED;
        goto rollback;
    }
    ret = register_maps(false);
    if (ret)
        goto rollback;
    ret = program_config(false);
    if (ret)
        goto rollback;
    if (fail_step == 2) {
        ret = -ECANCELED;
        goto rollback;
    }
    ret = commands(false);
    if (ret)
        goto rollback;
    ret = save_states();
    if (ret)
        goto rollback;
    ctx->changed = true;
    pr_info("pex_window: APPLIED: %d trees, %u GiB per slot, target VRAM BAR %u GiB\n",
            ctx->nt, slot_gib, bar_gib);
    goto out;
rollback:
    rr = restore_all();
    if (rr) {
        pr_crit("pex_window: rollback failed (%d); decoding remains disabled; reboot required\n", rr);
        ret = rr;
    } else {
        pr_warn("pex_window: transaction failed (%d), baseline restored\n", ret);
    }
out:
    return ret;
}

static int verify_rollback_layout(void)
{
    int i, j;
    for (i = 0; i < ctx->nm; i++) {
        struct mapping *m = &ctx->maps[i];
        struct resource *child;
        if (m->res->parent != m->new_parent || m->res->start != m->start || m->res->end != m->end)
            return -EBUSY;
        for (child = m->res->child; child; child = child->sibling) {
            for (j = 0; j < ctx->nm; j++)
                if (child == ctx->maps[j].res)
                    break;
            if (j == ctx->nm)
                return -EBUSY;
        }
    }
    return 0;
}

static int rollback_set(const char *value, const struct kernel_param *kp)
{
    bool requested;
    int ret = kstrtobool(value, &requested);
    if (ret || !requested)
        return ret ? ret : -EINVAL;
    mutex_lock(&operation_lock);
    if (!ctx || !ctx->changed) {
        ret = -EINVAL;
        goto out;
    }
    pci_lock_rescan_remove();
    ret = acquire_power();
    if (!ret)
        ret = lock_endpoints();
    if (!ret)
        ret = verify_rollback_layout();
    if (!ret)
        ret = restore_all();
    unlock_endpoints();
    release_power();
    pci_unlock_rescan_remove();
out:
    mutex_unlock(&operation_lock);
    return ret;
}
static const struct kernel_param_ops rollback_ops = { .set = rollback_set };
module_param_cb(rollback, &rollback_ops, NULL, 0200);
MODULE_PARM_DESC(rollback, "Write 1 with endpoint drivers detached to restore the pre-load layout");

static void cleanup(void)
{
    int i;
    if (!ctx)
        return;
    for (i = ctx->nn - 1; i >= 0; i--) {
        struct node *n = &ctx->nodes[i];
        if (n->powered)
            pm_runtime_put(&n->dev->dev);
        pci_dev_put(n->dev);
    }
    kvfree(ctx->nodes);
    kvfree(ctx->maps);
    kvfree(ctx->trees);
    kvfree(ctx);
    ctx = NULL;
}
static int __init pex_window_init(void)
{
    int ret;
    if (!IS_ENABLED(CONFIG_X86_64))
        return -ENOTSUPP;
    if (!is_power_of_2(bar_gib) || bar_gib > (1U << 17) ||
        !is_power_of_2(slot_gib) || slot_gib < bar_gib || empty_min_width > 32 || fail_step > 2)
        return -EINVAL;
    ctx = kvzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;
    pci_lock_rescan_remove();
    ret = discover();
    if (!ret && apply)
        ret = lock_endpoints();
    if (!ret)
        ret = make_plan();
    if (!ret && apply)
        ret = perform();
    if (!ret && !apply)
        pr_info("pex_window: DRY RUN: %d trees, %d devices, %d resources; no allocations changed\n",
                ctx->nt, ctx->nn, ctx->nm);
    unlock_endpoints();
    release_power();
    pci_unlock_rescan_remove();
    if (ret) {
        pr_err("pex_window: aborted (%d)\n", ret);
        cleanup();
    }
    return ret;
}
static void __exit pex_window_exit(void)
{
    if (ctx && ctx->changed)
        pr_info("pex_window: leaving verified PCI-owned allocations in place\n");
    cleanup();
}
module_init(pex_window_init);
module_exit(pex_window_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("dbsanfte contributors");
MODULE_DESCRIPTION("Transactional pre-driver PEX GPU MMIO window allocation");
MODULE_VERSION(PW_VERSION);
