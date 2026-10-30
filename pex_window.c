// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file pex_window.c
 * @brief Transactional allocation of prefetchable MMIO for PEX GPU trees.
 *
 * Discovery retains PCI device references and wakes the selected trees. Planning
 * constructs the entire replacement resource hierarchy without modifying it.
 * Application requires detached endpoint drivers, disables memory decoding and
 * bus mastering, replaces the resources, and verifies all configuration writes.
 * Any application failure attempts to restore the original hierarchy/registers.
 *
 * This helper runs before endpoint drivers; it does not replace pcieport. All
 * installed resource objects and their names belong to PCI devices, so a
 * successful layout survives unloading this module. The allocator changes only
 * 64-bit prefetchable windows and BARs, preserving bus numbers and other MMIO.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include "plan.h"
#include "transaction.h"

/** @brief Broadcom PCI vendor identifier for PEX880xx. */
#define PEX_VENDOR 0x1000
/** @brief PEX880xx device identifier. */
#define PEX_DEVICE 0xc010
/** @brief PCI-core resource index for the prefetchable bridge window. */
#define PREF_RES PCI_BRIDGE_PREF_MEM_WINDOW

/** @brief Explicit opt-in to changing allocation; otherwise plan only. */
static bool apply;
module_param(apply, bool, 0444);
MODULE_PARM_DESC(apply, "Apply allocation (default: read-only plan)");
/** @brief Transaction fault-injection point; zero disables injection. */
static unsigned int fail_step;
module_param(fail_step, uint, 0444);
MODULE_PARM_DESC(fail_step, "Testing only: inject failure at transaction step 1 or 2");
/** @brief Default requested VRAM BAR size in GiB. */
static unsigned int bar_gib = 32;
/** @brief Default address-space reservation per GPU slot in GiB. */
static unsigned int slot_gib = 64;
/** @brief Minimum maximum link width for reserving an empty downstream port. */
static unsigned int empty_min_width = 8;
module_param(bar_gib, uint, 0444);
module_param(slot_gib, uint, 0444);
module_param(empty_min_width, uint, 0444);
MODULE_PARM_DESC(bar_gib, "Requested VRAM BAR GiB (power of two, default 32)");
MODULE_PARM_DESC(slot_gib, "Address space per GPU/empty slot GiB (power of two, default 64)");
MODULE_PARM_DESC(empty_min_width,
                 "Reserve empty PEX downstream ports at least this wide (0 disables, default 8)");
/** @brief Optional exact root-port BDF selection. */
static char *roots;
module_param(roots, charp, 0444);
MODULE_PARM_DESC(roots, "Optional comma-separated root port BDFs; default discovers all PEX trees");

/** @brief One enumerated PCI function and its saved configuration. */
struct node {
    struct pci_dev *dev; /**< Retained PCI reference; valid until cleanup. */
    int parent;          /**< Parent node index, or -1 for a root port. */
    int tree;            /**< Owning tree index. */
    int vram_bar;        /**< VRAM BAR index, or -1 for non-GPU functions. */
    int rebar_offset;    /**< Resizable BAR control offset; zero if unchanged. */
    u64 slots;           /**< GPU/empty-slot capacity below this node. */
    u64 start;           /**< Planned inclusive prefetchable window base. */
    u64 end;             /**< Planned inclusive prefetchable window limit. */
    u64 cursor;          /**< Next address while placing immediate children. */
    u64 vram_bytes;      /**< Target VRAM size, retaining larger existing BARs. */
    bool powered;        /**< This transaction owns a runtime-PM reference. */
    bool locked;         /**< This transaction holds the endpoint device lock. */
    u16 command;         /**< Original command register, before decoding stops. */
    u32 rebar_old;       /**< Original ReBAR control word for rollback. */
    u32 rebar_new;       /**< Requested ReBAR control word. */
    u32 pref_regs[3];    /**< Original bridge base/limit and upper dwords. */
};

/** @brief Original and planned state of a PCI-owned resource. */
struct mapping {
    struct resource *res;        /**< PCI-owned object updated in place. */
    struct resource *old_parent; /**< Original resource parent, possibly NULL. */
    struct resource *new_parent; /**< Parent in the planned hierarchy. */
    const char *old_name;        /**< Original name retained for rollback. */
    resource_size_t old_start;   /**< Original inclusive base. */
    resource_size_t old_end;     /**< Original inclusive limit. */
    resource_size_t start;       /**< Planned inclusive base. */
    resource_size_t end;         /**< Planned inclusive limit. */
    unsigned long old_flags;     /**< Original resource flags. */
    unsigned long flags;         /**< Planned resource flags. */
    int node;                    /**< Index of the owning PCI function. */
    int bar;                     /**< BAR index or PREF_RES for a bridge window. */
    u32 low;                     /**< Original lower BAR dword, including type bits. */
    u32 high;                    /**< Original upper BAR dword. */
};

/** @brief Root-port subtree constrained by one existing host MMIO aperture. */
struct tree {
    int root;              /**< First node: this tree's root port. */
    int last;              /**< Exclusive end of this tree's node range. */
    struct resource *host; /**< Existing high-MMIO parent aperture. */
    resource_size_t start; /**< Planned inclusive root-window base. */
    resource_size_t end;   /**< Planned inclusive root-window limit. */
    resource_size_t bytes; /**< Aggregate per-slot reservation. */
};

/** @brief Transaction state, allocated to match the enumerated device count. */
struct context {
    struct node *nodes;   /**< Breadth-first nodes; parents precede children. */
    struct mapping *maps; /**< Resource mappings; parents precede children. */
    struct tree *trees;   /**< Discovered root-port subtrees. */
    int node_count;       /**< Number of initialized nodes. */
    int mapping_count;    /**< Number of initialized resource mappings. */
    int tree_count;       /**< Number of complete discovered trees. */
    int capacity;         /**< PCI enumeration count used to size storage. */
    bool changed;         /**< A successful layout can still be rolled back. */
};
/** @brief State retained until unload for optional explicit rollback. */
static struct context *ctx;
/** @brief Serialize explicit rollback requests against transaction state. */
static DEFINE_MUTEX(operation_lock);

/**
 * @brief Recognize supported Broadcom/PLX PEX PCIe functions.
 * @param d PCI function to identify.
 * @return True for recognized switch-family functions.
 */
static bool pex(struct pci_dev *d)
{
    return (d->vendor == PEX_VENDOR && d->device == PEX_DEVICE) ||
           (d->vendor == 0x10b5 && pci_is_pcie(d));
}
/**
 * @brief Identify display-class functions eligible for GPU BAR policy.
 * @param d PCI function to inspect.
 * @return True when the base class is display.
 */
static bool graphics(struct pci_dev *d)
{
    return (d->class >> 16) == PCI_BASE_CLASS_DISPLAY;
}
/**
 * @brief Check that a resource is 64-bit prefetchable memory.
 * @param r Resource descriptor to inspect.
 * @return True only when all three required resource flags are set.
 */
static bool pref64(struct resource *r)
{
    return (r->flags & (IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_MEM_64)) ==
           (IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_MEM_64);
}
/**
 * @brief Read a configuration word and normalize PCI errors to errno.
 * @param d PCI function to read.
 * @param pos Byte offset in configuration space.
 * @param v Output word, valid on success.
 * @return Zero on success or -EIO on a failed configuration access.
 */
static int cfg_word(struct pci_dev *d, int pos, u16 *v)
{
    return pci_read_config_word(d, pos, v) ? -EIO : 0;
}
/**
 * @brief Read a configuration dword and normalize PCI errors to errno.
 * @param d PCI function to read.
 * @param pos Byte offset in configuration space.
 * @param v Output dword, valid on success.
 * @return Zero on success or -EIO on a failed configuration access.
 */
static int cfg_dword(struct pci_dev *d, int pos, u32 *v)
{
    return pci_read_config_dword(d, pos, v) ? -EIO : 0;
}

/**
 * @brief Retain, resume, classify, and snapshot one PCI function.
 * @param d Function discovered under a selected root.
 * @param parent Parent node index, or -1 for a root port.
 * @param ti Owning tree index.
 * @return The new node index, or a negative errno; cleanup owns partial state.
 */
static int gather_one(struct pci_dev *d, int parent, int ti)
{
    struct node *n;
    u32 link;
    int idx, ret;
    if (ctx->node_count == ctx->capacity)
        return -EOVERFLOW; /* enumeration changed despite rescan/remove lock */
    idx = ctx->node_count++;
    n = &ctx->nodes[idx];
    n->dev = pci_dev_get(d);
    n->parent = parent;
    n->tree = ti;
    n->vram_bar = -1;
    ret = pm_runtime_resume_and_get(&d->dev);
    if (ret < 0)
        return ret;
    n->powered = true;
    if (cfg_word(d, PCI_COMMAND, &n->command))
        return -EIO;
    if (pci_is_bridge(d)) {
        if (!pci_is_pcie(d) || !d->subordinate ||
            cfg_dword(d, PCI_PREF_MEMORY_BASE, &n->pref_regs[0]) ||
            cfg_dword(d, PCI_PREF_BASE_UPPER32, &n->pref_regs[1]) ||
            cfg_dword(d, PCI_PREF_LIMIT_UPPER32, &n->pref_regs[2]))
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
        n->vram_bar = d->vendor == 0x10de ? 1 : 0;
        n->slots = 1;
    } else if (!((d->class >> 8) == PCI_CLASS_MULTIMEDIA_HD_AUDIO &&
                 (d->vendor == 0x1002 || d->vendor == 0x10de)) &&
               !(pex(d) && (d->class >> 8) == PCI_CLASS_STORAGE_OTHER)) {
        pci_err(d,
                "pex_window: foreign endpoint in target tree; use roots= to restrict discovery\n");
        return -ENODEV;
    }
    return idx;
}

/**
 * @brief Mark a subtree when pci_walk_bus encounters a PEX bridge.
 * @param d Function visited by the PCI walker.
 * @param data Pointer to the boolean discovery result.
 * @return Zero to continue walking the bus.
 */
static int find_pex(struct pci_dev *d, void *data)
{
    bool *found = data;
    if (pex(d) && pci_is_bridge(d))
        *found = true;
    return 0;
}

/**
 * @brief Match a root port against the optional comma-separated filter.
 * @param d Candidate root-port function.
 * @return True for an exact BDF match, or when no filter was supplied.
 */
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

/**
 * @brief Discover PEX trees and aggregate descendant slot capacity.
 * @return Zero when at least one eligible tree was gathered, otherwise a negative errno.
 *
 * The caller holds the PCI rescan/remove lock. Storage is sized from the full
 * device enumeration; breadth-first traversal and reverse aggregation avoid
 * recursion and any fixed switch, device, or GPU-count ceiling.
 */
static int discover(void)
{
    struct pci_dev *d = NULL, *child;
    int ret, i, count = 0;
    for_each_pci_dev(d)
    {
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
    for_each_pci_dev(d)
    {
        struct tree *t;
        bool found = false;
        if (!pci_is_root_bus(d->bus) || !pci_is_bridge(d) || !d->subordinate ||
            pci_pcie_type(d) != PCI_EXP_TYPE_ROOT_PORT || !selected_root(d))
            continue;
        pci_walk_bus(d->subordinate, find_pex, &found);
        if (!found)
            continue;
        t = &ctx->trees[ctx->tree_count];
        ret = gather_one(d, -1, ctx->tree_count);
        if (ret < 0)
            goto fail;
        t->root = ret;
        /* Breadth first, avoiding recursion even for deeply cascaded switches. */
        for (i = t->root; i < ctx->node_count; i++) {
            struct pci_bus *bus = ctx->nodes[i].dev->subordinate;
            if (!bus)
                continue;
            list_for_each_entry(child, &bus->devices, bus_list)
            {
                ret = gather_one(child, i, ctx->tree_count);
                if (ret < 0)
                    goto fail;
            }
        }
        t->last = ctx->node_count;
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
        ctx->tree_count++;
    }
    return ctx->tree_count ? 0 : -ENODEV;
fail:
    pci_dev_put(d);
    return ret;
}

/**
 * @brief Plan VRAM enlargement while preserving a larger existing BAR.
 * @param n GPU node whose capability and resource will be checked.
 * @return Zero if no resize is needed or the requested size is supported; negative errno otherwise.
 */
static int rebar_plan(struct node *n)
{
    struct pci_dev *d = n->dev;
    struct resource *r = &d->resource[n->vram_bar];
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
        if (cfg_dword(d, pos + PCI_REBAR_CTRL, &ctrl) || cfg_dword(d, pos + PCI_REBAR_CAP, &cap))
            return -EIO;
        if ((ctrl & PCI_REBAR_CTRL_BAR_IDX) != n->vram_bar)
            continue;
        if (!(cap & BIT(size + 4)))
            return -ENOTSUPP;
        n->rebar_offset = pos + PCI_REBAR_CTRL;
        n->rebar_old = ctrl;
        n->rebar_new = (ctrl & ~PCI_REBAR_CTRL_BAR_SIZE) | (size << PCI_REBAR_CTRL_BAR_SHIFT);
        return 0;
    }
    return -ENOTSUPP;
}

/**
 * @brief Snapshot a resource and append its proposed replacement.
 * @param ni Owning node index.
 * @param bar Endpoint BAR index or PREF_RES.
 * @param parent Planned resource parent.
 * @param start Planned inclusive start address.
 * @param end Planned inclusive end address.
 * @return Zero on success, or negative errno for an unsupported or stale resource.
 */
static int add_map(int ni, int bar, struct resource *parent, resource_size_t start,
                   resource_size_t end)
{
    struct mapping *m;
    struct pci_dev *d = ctx->nodes[ni].dev;
    struct resource *r = &d->resource[bar];
    if (ctx->mapping_count == ctx->capacity * PCI_STD_NUM_BARS || start > end)
        return -E2BIG;
    m = &ctx->maps[ctx->mapping_count++];
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
    m->flags = bar == PREF_RES ? (IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_MEM_64 | 1)
                               : (r->flags & ~(IORESOURCE_UNSET | IORESOURCE_STARTALIGN));
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
    pci_info(d, "pex_window: plan resource %d [%#llx-%#llx] (%llu MiB)\n", bar, (u64)start,
             (u64)end, (u64)(end - start + 1) >> 20);
    return 0;
}

/**
 * @brief Place every descendant window and GPU BAR inside a root reservation.
 * @param t Tree with a root interval selected by make_plan().
 * @return Zero if the entire hierarchy fits, otherwise a negative errno.
 */
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
                (n->pref_regs[0] & 0x000f000f) != 0x00010001)
                return -ENOTSUPP;
            for (i = 0; i < PCI_STD_NUM_BARS; i++)
                if (d->resource[i].flags & IORESOURCE_PREFETCH)
                    return -ENOTSUPP;
            ret = add_map(ni, PREF_RES, pr, n->start, n->end);
            if (ret)
                return ret;
        } else if (n->vram_bar >= 0) {
            ret = rebar_plan(n);
            if (ret)
                return ret;
            for (i = -1; i < PCI_STD_NUM_BARS; i++) {
                int bar = i < 0 ? n->vram_bar : i;
                struct resource *r = &d->resource[bar];
                if ((i >= 0 && bar == n->vram_bar) || !(r->flags & IORESOURCE_PREFETCH))
                    continue;
                if (!pref64(r))
                    return -ENOTSUPP;
                bytes = bar == n->vram_bar ? n->vram_bytes : resource_size(r);
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

/**
 * @brief Identify old root windows released by this transaction.
 * @param r Host-aperture child resource to examine.
 * @return True when the resource is a selected root-port window.
 */
static bool moving_root(struct resource *r)
{
    int i;
    for (i = 0; i < ctx->tree_count; i++)
        if (r == &ctx->nodes[ctx->trees[i].root].dev->resource[PREF_RES])
            return true;
    return false;
}

/**
 * @brief Allocate root intervals jointly and verify resource ownership.
 * @return Zero for a complete plan, or negative errno without allocation changes.
 *
 * Each host-aperture search includes immovable firmware reservations and
 * earlier plans sharing that aperture. Old selected root windows are excluded.
 * Existing resource children must also participate, preventing orphaned regions.
 */
static int make_plan(void)
{
    int i, j, ret;
    for (i = 0; i < ctx->tree_count; i++) {
        struct tree *t = &ctx->trees[i];
        struct resource *other;
        struct pw_interval *used;
        unsigned int count = 0, k = 0;
        for (other = t->host->child; other; other = other->sibling)
            if (!moving_root(other))
                count++;
        used = kvcalloc((size_t)count + ctx->tree_count, sizeof(*used), GFP_KERNEL);
        if (!used)
            return -ENOMEM;
        for (other = t->host->child; other; other = other->sibling)
            if (!moving_root(other))
                used[k++] = (struct pw_interval){other->start, other->end};
        for (j = 0; j < i; j++)
            if (ctx->trees[j].host == t->host)
                used[k++] = (struct pw_interval){ctx->trees[j].start, ctx->trees[j].end};
        ret = pw_find_space(t->host->start, t->host->end, t->bytes, (u64)slot_gib * PW_GIB, used, k,
                            &t->start, &t->end);
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
    for (i = 0; i < ctx->mapping_count; i++) {
        struct resource *child;
        if (!apply && ctx->maps[i].bar < PCI_STD_NUM_BARS)
            continue;
        for (child = ctx->maps[i].res->child; child; child = child->sibling) {
            for (j = 0; j < ctx->mapping_count; j++)
                if (ctx->maps[j].res == child)
                    break;
            if (j == ctx->mapping_count)
                return -EBUSY;
        }
    }
    return 0;
}

/**
 * @brief Lock all endpoints and reject any bound driver.
 * @return Zero if all endpoints are detached; negative errno otherwise.
 * Partial locks remain recorded and must be released by unlock_endpoints().
 */
static int lock_endpoints(void)
{
    int i;
    for (i = 0; i < ctx->node_count; i++) {
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
/**
 * @brief Release recorded endpoint locks in reverse traversal order.
 */
static void unlock_endpoints(void)
{
    int i;
    for (i = ctx->node_count - 1; i >= 0; i--)
        if (ctx->nodes[i].locked) {
            device_unlock(&ctx->nodes[i].dev->dev);
            ctx->nodes[i].locked = false;
        }
}
/**
 * @brief Drop transaction-owned runtime-PM references in child-first order.
 */
static void release_power(void)
{
    int i;
    for (i = ctx->node_count - 1; i >= 0; i--) {
        struct node *n = &ctx->nodes[i];
        if (n->powered) {
            pm_runtime_put(&n->dev->dev);
            n->powered = false;
        }
    }
}
/**
 * @brief Wake retained devices before a later explicit rollback.
 * @return Zero if every device resumed and remains present, otherwise a negative errno.
 * The caller releases partial references with release_power().
 */
static int acquire_power(void)
{
    int i, ret;
    for (i = 0; i < ctx->node_count; i++) {
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
/**
 * @brief Command-word write adapter for the shared readback helper.
 * @param opaque PCI function being updated.
 * @param value New command word.
 * @return PCI configuration access status.
 */
static int write_command(void *opaque, pw_u16 value)
{
    return pci_write_config_word(opaque, PCI_COMMAND, value);
}
/**
 * @brief Command-word read adapter for the shared readback helper.
 * @param opaque PCI function being read.
 * @param value Output command word.
 * @return Zero on success, -EIO on read failure.
 */
static int read_command(void *opaque, pw_u16 *value)
{
    return cfg_word(opaque, PCI_COMMAND, value);
}

/**
 * @brief Disable memory decoding/bus mastering or restore saved commands.
 * @param opaque Backend context, unused because the module retains one transaction.
 * @param disable True to stop decoding and DMA; false to restore original command words.
 * @return Zero if every write reads back correctly, or -EIO otherwise.
 */
static int commands(void *opaque, bool disable)
{
    int i, ret = 0;
    for (i = 0; i < ctx->node_count; i++) {
        struct node *n = &ctx->nodes[i];
        u16 cmd = n->command;
        if (disable)
            cmd &= ~(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        if (pw_verified_word(write_command, read_command, n->dev, cmd))
            ret = -EIO;
    }
    return ret;
}
/**
 * @brief Drain outstanding requests after the shared helper disables mastering.
 * @param opaque Backend context, unused for the retained module transaction.
 * @return Zero after every endpoint drained, or -EBUSY on timeout.
 */
static int drain_pending(void *opaque)
{
    int i;
    for (i = 0; i < ctx->node_count; i++) {
        struct pci_dev *d = ctx->nodes[i].dev;
        if (!pci_is_bridge(d) && !pci_wait_for_pending_transaction(d))
            return -EBUSY;
    }
    return 0;
}

/**
 * @brief Unlink mapped resources in child-before-parent order.
 * @param opaque Backend context, unused because the module retains one transaction.
 * @return Zero on success or -EBUSY if a resource cannot be released.
 */
static int release_maps(void *opaque)
{
    int i;
    for (i = ctx->mapping_count - 1; i >= 0; i--)
        if (ctx->maps[i].res->parent && release_resource(ctx->maps[i].res))
            return -EBUSY;
    return 0;
}
/**
 * @brief Register resource objects in parent-before-child order.
 * @param opaque Backend context, unused because the module retains one transaction.
 * @param restore True for the original hierarchy, false for the planned hierarchy.
 * @return Zero on success or -ENOSPC on a resource conflict.
 */
static int register_maps(void *opaque, bool restore)
{
    int i;
    for (i = 0; i < ctx->mapping_count; i++) {
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
/**
 * @brief Write and read back ReBAR controls, BARs, and bridge windows.
 * @param opaque Backend context, unused because the module retains one transaction.
 * @param restore True to restore captured register words; false to apply the plan.
 * @return Zero only when every register matches its intended value, otherwise -EIO.
 * Memory decoding must already be disabled on all affected devices.
 */
static int program_config(void *opaque, bool restore)
{
    int i, error = 0;
    u32 readback;
    for (i = 0; i < ctx->node_count; i++) {
        struct node *n = &ctx->nodes[i];
        u32 ctrl = restore ? n->rebar_old : n->rebar_new;
        if (!n->rebar_offset)
            continue;
        if (pci_write_config_dword(n->dev, n->rebar_offset, ctrl) ||
            cfg_dword(n->dev, n->rebar_offset, &readback) || readback != ctrl)
            error = -EIO;
    }
    for (i = 0; i < ctx->mapping_count; i++) {
        struct mapping *m = &ctx->maps[i];
        struct node *n = &ctx->nodes[m->node];
        struct pci_dev *d = n->dev;
        u32 regs[3], offsets[3];
        int count, j;
        if (m->bar == PREF_RES) {
            regs[0] = restore ? n->pref_regs[1] : upper_32_bits(m->start);
            regs[1] = restore ? n->pref_regs[2] : upper_32_bits(m->end);
            regs[2] =
                restore ? n->pref_regs[0]
                        : (((m->start >> 16) & 0xfff0) | 1 | ((m->end & 0xfff00000ULL) | 0x10000));
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
/**
 * @brief Refresh PCI-core saved state for subsequent runtime resume.
 * @param opaque Backend context, unused because the module retains one transaction.
 * @return Zero when every state was saved, or -EIO on failure.
 */
static int save_states(void *opaque)
{
    int i, ret = 0;
    for (i = 0; i < ctx->node_count; i++)
        if (pci_save_state(ctx->nodes[i].dev))
            ret = -EIO;
    return ret;
}
/** @brief Production backend for the transaction engine tested by CTest. */
static const struct pw_operations operations = {
    .commands = commands,
    .drain = drain_pending,
    .release = release_maps,
    .register_layout = register_maps,
    .program = program_config,
    .save = save_states,
};

/**
 * @brief Report whether final error-containment writes were verified.
 * @param error Result of the final command-register disable operation.
 */
static void report_containment(int error)
{
    if (error)
        pr_crit(
            "pex_window: cannot verify decoding/DMA disable; keep endpoints detached and reboot\n");
    else
        pr_err("pex_window: decoding and bus mastering disabled and verified\n");
}
/**
 * @brief Apply the plan using the shared and fault-tested transaction ordering.
 * @return Zero after verified application; operation or recovery errno otherwise.
 */
static int perform(void)
{
    struct pw_outcome outcome;
    int ret = pw_apply(&operations, ctx, fail_step, &outcome);
    if (!ret) {
        ctx->changed = true;
        pr_info("pex_window: APPLIED: %d trees, %u GiB per slot, target VRAM BAR %u GiB\n",
                ctx->tree_count, slot_gib, bar_gib);
    } else if (outcome.restored) {
        pr_info("pex_window: baseline resource layout restored and verified\n");
        pr_warn("pex_window: transaction failed (%d), baseline restored\n", ret);
    } else {
        if (outcome.containment)
            report_containment(outcome.disable_error);
        pr_crit("pex_window: rollback failed (%d); keep endpoints detached and reboot\n", ret);
    }
    return ret;
}

/**
 * @brief Check whether a resource belongs to this transaction.
 * @param resource Resource pointer to locate.
 * @return True for a mapped BAR or bridge window.
 */
static bool mapped_resource(struct resource *resource)
{
    int i;
    for (i = 0; i < ctx->mapping_count; i++)
        if (ctx->maps[i].res == resource)
            return true;
    return false;
}

/**
 * @brief Reject rollback if resource ranges or ownership have changed.
 * @param opaque Backend context, unused for the retained module transaction.
 * @return Zero when the saved applied layout is intact; -EBUSY otherwise.
 * New hotplug resources and driver-owned children are never silently detached.
 * Original ranges must still be available before the applied layout is released.
 */
static int verify_rollback_layout(void *opaque)
{
    int i;
    for (i = 0; i < ctx->mapping_count; i++) {
        struct mapping *m = &ctx->maps[i];
        struct resource *child;
        if (m->res->parent != m->new_parent || m->res->start != m->start || m->res->end != m->end)
            return -EBUSY;
        for (child = m->res->child; child; child = child->sibling) {
            if (!mapped_resource(child))
                return -EBUSY;
        }
        if (m->old_parent) {
            struct pw_interval *used;
            unsigned int count = 0;
            u64 parent_start = m->old_parent->start, parent_end = m->old_parent->end;
            int j, error;
            for (j = 0; j < ctx->mapping_count; j++) {
                if (ctx->maps[j].res == m->old_parent) {
                    parent_start = ctx->maps[j].old_start;
                    parent_end = ctx->maps[j].old_end;
                    break;
                }
            }
            for (child = m->old_parent->child; child; child = child->sibling)
                if (!mapped_resource(child))
                    count++;
            used = kvcalloc((size_t)count + 1, sizeof(*used), GFP_KERNEL);
            if (!used)
                return -ENOMEM;
            count = 0;
            for (child = m->old_parent->child; child; child = child->sibling)
                if (!mapped_resource(child))
                    used[count++] = (struct pw_interval){child->start, child->end};
            error = pw_original_range_free(m->old_start, m->old_end, parent_start, parent_end, used,
                                           count);
            kvfree(used);
            if (error)
                return -EBUSY;
        }
    }
    return 0;
}

/**
 * @brief Handle an explicit write to the rollback module parameter.
 * @param value Boolean text; only a true value requests rollback.
 * @param kp Kernel parameter descriptor, unused.
 * @return Zero on verified rollback, otherwise a negative errno.
 */
static int rollback_set(const char *value, const struct kernel_param *kp)
{
    struct pw_rollback_outcome outcome;
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
    if (!ret) {
        ret = pw_rollback(&operations, ctx, verify_rollback_layout, &outcome);
        if (!ret) {
            ctx->changed = false;
            pr_info("pex_window: baseline resource layout restored and verified\n");
        } else if (outcome.attempted) {
            if (outcome.fallback_error)
                report_containment(outcome.disable_error);
            else
                pr_warn(
                    "pex_window: applied layout recovered and verified after rollback failure\n");
        }
    }
    unlock_endpoints();
    release_power();
    pci_unlock_rescan_remove();
out:
    mutex_unlock(&operation_lock);
    return ret;
}
/** @brief Write-only parameter operations for explicit rollback. */
static const struct kernel_param_ops rollback_ops = {.set = rollback_set};
module_param_cb(rollback, &rollback_ops, NULL, 0200);
MODULE_PARM_DESC(rollback, "Write 1 with endpoint drivers detached to restore the pre-load layout");

/**
 * @brief Release retained references and transaction storage without moving resources.
 */
static void cleanup(void)
{
    int i;
    if (!ctx)
        return;
    for (i = ctx->node_count - 1; i >= 0; i--) {
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
/**
 * @brief Validate policy, discover and plan trees, and optionally apply.
 * @return Zero when the module can remain loaded, otherwise a negative errno.
 */
static int __init pex_window_init(void)
{
    int ret;
    if (!IS_ENABLED(CONFIG_X86_64))
        return -ENOTSUPP;
    if (!is_power_of_2(bar_gib) || bar_gib > (1U << 17) || !is_power_of_2(slot_gib) ||
        slot_gib < bar_gib || empty_min_width > 32 || fail_step > 2)
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
                ctx->tree_count, ctx->node_count, ctx->mapping_count);
    unlock_endpoints();
    release_power();
    pci_unlock_rescan_remove();
    if (ret) {
        pr_err("pex_window: aborted (%d)\n", ret);
        cleanup();
    }
    return ret;
}
/**
 * @brief Unload bookkeeping while preserving successful PCI-owned allocations.
 */
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
