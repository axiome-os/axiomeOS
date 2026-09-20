#include "axiomefs.h"
#include "vfs.h"
#include "ide.h"
#include "slab.h"
#include "printk.h"
#include "string.h"
#include "sched.h"
#include <stddef.h>

#define AXFS_SECTORS_PER_BLOCK (AXFS_BLOCK_SIZE / 512)

/* ---- checksum (FNV-1a 64) ---- */
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

static uint64_t afs_fnv(const uint8_t *p, size_t len, uint64_t h)
{
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= FNV_PRIME; }
    return h;
}

/* Checksum covers the whole block except the 8-byte checksum field at [24,32). */
static uint64_t afs_checksum_of(const uint8_t *blk)
{
    uint64_t h = FNV_OFFSET;
    h = afs_fnv(blk, 24, h);
    h = afs_fnv(blk + 32, AXFS_BLOCK_SIZE - 32, h);
    return h;
}

static void afs_set_checksum(uint8_t *blk)
{
    struct axfs_obj_hdr *h = (struct axfs_obj_hdr *)blk;
    h->checksum = 0;
    h->checksum = afs_checksum_of(blk);
}

/* ---- on-disk block IO ---- */
struct axfs_state {
    struct block_dev *bd;
    uint32_t part_start;        /* LBA of the volume (superfloppy => 0) */
    uint64_t total_blocks;
    uint64_t root_inode_block;
    uint64_t free_bitmap_block;
    uint64_t transaction_id;
    struct vnode *root;
};

static void afs_read_block(struct axfs_state *st, uint64_t block, void *buf)
{
    blk_read(st->bd, (uint64_t)st->part_start + block * AXFS_SECTORS_PER_BLOCK,
             AXFS_SECTORS_PER_BLOCK, buf);
}
static void afs_write_block(struct axfs_state *st, uint64_t block, const void *buf)
{
    blk_write(st->bd, (uint64_t)st->part_start + block * AXFS_SECTORS_PER_BLOCK,
              AXFS_SECTORS_PER_BLOCK, buf);
}

/* ---- free-space bitmap (single block; covers 32768 blocks = 128 MB) ---- */
static uint64_t afs_alloc_block(struct axfs_state *st)
{
    uint8_t bm[AXFS_BLOCK_SIZE];
    afs_read_block(st, st->free_bitmap_block, bm);
    for (uint64_t i = 0; i < st->total_blocks; i++)
    {
        uint64_t byte = i / 8, bit = i % 8;
        if (!(bm[byte] & (1u << bit)))
        {
            bm[byte] |= (1u << bit);
            afs_write_block(st, st->free_bitmap_block, bm);
            return i;
        }
    }
    return 0; /* exhausted */
}
static void afs_free_block(struct axfs_state *st, uint64_t block)
{
    uint8_t bm[AXFS_BLOCK_SIZE];
    afs_read_block(st, st->free_bitmap_block, bm);
    bm[block / 8] &= ~(1u << (block % 8));
    afs_write_block(st, st->free_bitmap_block, bm);
}

/* ---- superblock checkpoint (primary + backup) ---- */
static void afs_write_super(struct axfs_state *st)
{
    uint8_t blk[AXFS_BLOCK_SIZE];
    afs_read_block(st, 0, blk);
    struct axfs_super *s = (struct axfs_super *)blk;
    __builtin_memcpy(s->magic, AXFS_MAGIC, 8);
    s->block_size = AXFS_BLOCK_SIZE;
    s->total_blocks = st->total_blocks;
    s->root_inode = st->root_inode_block;
    s->free_bitmap_block = st->free_bitmap_block;
    s->transaction_id = st->transaction_id;
    s->hdr.type = AXFS_OBJ_SUPER;
    s->hdr.object_id = 0;
    s->hdr.transaction_id = st->transaction_id;
    afs_set_checksum(blk);
    afs_write_block(st, 0, blk);
    afs_write_block(st, 1, blk); /* checkpoint backup */
}

static void afs_read_inode(struct axfs_state *st, uint64_t block, struct axfs_inode *out)
{
    uint8_t blk[AXFS_BLOCK_SIZE];
    afs_read_block(st, block, blk);
    __builtin_memcpy(out, blk, sizeof(*out));
}

/* ---- directory mutation helpers (in-place; inode blocks are stable) ---- */
static void afs_dir_add_entry(struct axfs_state *st, uint64_t dir_block,
                               const char *name, uint64_t child, uint8_t type)
{
    struct axfs_inode di;
    afs_read_inode(st, dir_block, &di);

    /* Find an existing data block that still has a free slot. */
    int target = -1, slot = -1;
    for (uint32_t e = 0; e < di.extent_count; e++)
    {
        uint8_t blk[AXFS_BLOCK_SIZE];
        afs_read_block(st, di.extents[e].physical_block, blk);
        for (int i = 0; i < 15; i++)
        {
            struct axfs_dent *d = (struct axfs_dent *)(blk + i * sizeof(struct axfs_dent));
            if (d->inode_id == 0) { target = (int)e; slot = i; break; }
        }
        if (target >= 0) break;
    }

    if (target >= 0)
    {
        /* Reuse the existing data block: write the new entry in place. */
        uint8_t nb[AXFS_BLOCK_SIZE];
        afs_read_block(st, di.extents[target].physical_block, nb);
        struct axfs_dent *nd = (struct axfs_dent *)(nb + slot * sizeof(struct axfs_dent));
        nd->inode_id = (uint32_t)child;
        nd->type = type;
        nd->namelen = (uint8_t)__builtin_strlen(name);
        __builtin_memcpy(nd->name, name, nd->namelen);
        afs_write_block(st, di.extents[target].physical_block, nb);
        return;
    }

    /* Directory is full: allocate one new data block and grow the inode
       in place (the inode keeps the same block number). */
    if (di.extent_count >= AXFS_MAX_EXTENTS)
        return;
    uint64_t new_data = afs_alloc_block(st);
    uint8_t nb[AXFS_BLOCK_SIZE];
    __builtin_memset(nb, 0, AXFS_BLOCK_SIZE);
    struct axfs_dent *nd = (struct axfs_dent *)nb;
    nd->inode_id = (uint32_t)child;
    nd->type = type;
    nd->namelen = (uint8_t)__builtin_strlen(name);
    __builtin_memcpy(nd->name, name, nd->namelen);
    afs_write_block(st, new_data, nb);

    di.extents[di.extent_count].physical_block = new_data;
    di.extents[di.extent_count].block_count = 1;
    di.extents[di.extent_count].reference_count = 1;
    di.extent_count++;
    di.hdr.transaction_id = ++st->transaction_id;
    di.hdr.type = AXFS_OBJ_INODE;
    uint8_t nblk[AXFS_BLOCK_SIZE];
    __builtin_memcpy(nblk, &di, sizeof(di));
    afs_set_checksum(nblk);
    afs_write_block(st, dir_block, nblk);
}

static void afs_dir_remove_entry(struct axfs_state *st, uint64_t dir_block,
                                  const char *name)
{
    struct axfs_inode di;
    afs_read_inode(st, dir_block, &di);
    size_t nl = __builtin_strlen(name);
    for (uint32_t e = 0; e < di.extent_count; e++)
    {
        uint8_t blk[AXFS_BLOCK_SIZE];
        afs_read_block(st, di.extents[e].physical_block, blk);
        for (int i = 0; i < 15; i++)
        {
            struct axfs_dent *d = (struct axfs_dent *)(blk + i * sizeof(struct axfs_dent));
            if (d->inode_id && d->namelen &&
                __builtin_strncmp(d->name, name, d->namelen) == 0 && d->namelen == nl)
            {
                d->inode_id = 0;
                d->namelen = 0;
                d->name[0] = 0;
                afs_write_block(st, di.extents[e].physical_block, blk);
                return;
            }
        }
    }
}

/* ---- VFS operations ---- */
static struct vnode *axfs_make_vnode(struct vfs_super *sb, struct axfs_state *st,
                                     uint64_t inode_block)
{
    struct axfs_inode in;
    afs_read_inode(st, inode_block, &in);
    struct vnode *n = kmalloc(sizeof(struct vnode));
    if (!n) return 0;
    __builtin_memset(n, 0, sizeof(*n));
    n->sb = sb;
    n->type = (in.in_type == AXFS_INODE_DIR) ? VFS_DIR : VFS_FILE;
    n->size = (size_t)in.size;
    n->priv = (void *)(uintptr_t)inode_block;
    /* POSIX ownership / permission state (user rank system). */
    n->v_uid = in.uid;
    n->v_gid = in.gid;
    n->v_mode = (uint16_t)(in.permissions & 07777);
    int nl = 0;
    while (nl < MAX_NAME && in.hdr.object_id && 0) nl++; /* unused */
    (void)nl;
    return n;
}

static struct vnode *axfs_lookup(struct vfs_super *sb, struct vnode *dir,
                                 const char *relpath)
{
    struct axfs_state *st = sb->priv;
    const char *p = relpath;
    if (p[0] == '/') p++;
    uint64_t cur = (uint64_t)(uintptr_t)dir->priv;
    while (*p)
    {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t sl = (size_t)(p - seg);
        if (sl > MAX_NAME) sl = MAX_NAME;
        char comp[MAX_NAME + 1];
        for (size_t i = 0; i < sl; i++) comp[i] = seg[i];
        comp[sl] = 0;

        struct axfs_inode di;
        afs_read_inode(st, cur, &di);
        if (di.in_type != AXFS_INODE_DIR) return 0;
        uint64_t next = 0;
        for (uint32_t e = 0; e < di.extent_count; e++)
        {
            uint8_t blk[AXFS_BLOCK_SIZE];
            afs_read_block(st, di.extents[e].physical_block, blk);
            for (int i = 0; i < 15; i++)
            {
                struct axfs_dent *d = (struct axfs_dent *)(blk + i * sizeof(struct axfs_dent));
                if (d->inode_id && d->namelen &&
                    __builtin_strncmp(d->name, comp, d->namelen) == 0 &&
                    d->namelen == sl)
                {
                    next = d->inode_id;
                    break;
                }
            }
            if (next) break;
        }
        if (!next) return 0;
        cur = next;
    }
    return axfs_make_vnode(sb, st, cur);
}

static size_t axfs_read(struct vfs_super *sb, struct vnode *node, size_t off,
                        void *buf, size_t len)
{
    struct axfs_state *st = sb->priv;
    struct axfs_inode in;
    afs_read_inode(st, (uint64_t)(uintptr_t)node->priv, &in);
    if (off >= in.size) return 0;
    size_t avail = (size_t)in.size - off;
    size_t r = (len < avail) ? len : avail;
    size_t bs = AXFS_BLOCK_SIZE;
    uint64_t first = off / bs, last = (off + r - 1) / bs;
    size_t done = 0;
    for (uint64_t L = first; L <= last && L < in.extent_count; L++)
    {
        uint8_t blk[AXFS_BLOCK_SIZE];
        afs_read_block(st, in.extents[L].physical_block, blk);
        size_t lo = (L == first) ? (off % bs) : 0;
        size_t hi = (L == last) ? ((off + r - 1) % bs) : (bs - 1);
        size_t clen = hi - lo + 1;
        __builtin_memcpy((uint8_t *)buf + done, blk + lo, clen);
        done += clen;
    }
    return done;
}

static size_t axfs_write(struct vfs_super *sb, struct vnode *node, size_t off,
                         const void *buf, size_t len)
{
    struct axfs_state *st = sb->priv;
    uint64_t inode_block = (uint64_t)(uintptr_t)node->priv;
    struct axfs_inode in;
    afs_read_inode(st, inode_block, &in);
    size_t bs = AXFS_BLOCK_SIZE;
    uint64_t first = off / bs, last = (off + len - 1) / bs;

    for (uint64_t L = first; L <= last; L++)
    {
        size_t lo = (L == first) ? (off % bs) : 0;
        size_t hi = (L == last) ? ((off + len - 1) % bs) : (bs - 1);
        size_t clen = hi - lo + 1;
        uint64_t phys = (L < in.extent_count) ? in.extents[L].physical_block : 0;
        uint64_t newphys = phys ? phys : afs_alloc_block(st);
        uint8_t blk[AXFS_BLOCK_SIZE];
        __builtin_memset(blk, 0, AXFS_BLOCK_SIZE);
        if (phys) afs_read_block(st, phys, blk);
        size_t src = L * bs + lo - off;
        __builtin_memcpy(blk + lo, (const uint8_t *)buf + src, clen);
        afs_write_block(st, newphys, blk);
        if (L < in.extent_count)
            in.extents[L].physical_block = newphys;   /* same block */
        else
        {
            in.extents[in.extent_count].physical_block = newphys;
            in.extents[in.extent_count].block_count = 1;
            in.extents[in.extent_count].reference_count = 1;
            in.extent_count++;
        }
    }
    if (off + len > in.size) in.size = off + len;
    in.modified = in.modified + 1;
    in.hdr.transaction_id = ++st->transaction_id;
    in.hdr.type = AXFS_OBJ_INODE;

    uint8_t nblk[AXFS_BLOCK_SIZE];
    __builtin_memcpy(nblk, &in, sizeof(in));
    afs_set_checksum(nblk);
    afs_write_block(st, inode_block, nblk);   /* rewrite the SAME inode block */
    node->size = (size_t)in.size;
    /* node->priv (inode block) is unchanged, so cached vnodes stay valid. */
    return len;
}

static int axfs_list(struct vfs_super *sb, struct vnode *dir,
                     struct vfs_dirent *ents, int max)
{
    struct axfs_state *st = sb->priv;
    struct axfs_inode di;
    afs_read_inode(st, (uint64_t)(uintptr_t)dir->priv, &di);
    int count = 0;
    if (count < max) { ents[count].type = DT_DIR; __builtin_memset(ents[count].name, 0, MAX_NAME + 1); ents[count].name[0] = '.'; }
    count++;
    if (count < max) { ents[count].type = DT_DIR; __builtin_memset(ents[count].name, 0, MAX_NAME + 1); ents[count].name[0] = '.'; ents[count].name[1] = '.'; }
    count++;
    for (uint32_t e = 0; e < di.extent_count; e++)
    {
        uint8_t blk[AXFS_BLOCK_SIZE];
        afs_read_block(st, di.extents[e].physical_block, blk);
        for (int i = 0; i < 15; i++)
        {
            struct axfs_dent *d = (struct axfs_dent *)(blk + i * sizeof(struct axfs_dent));
            if (!d->inode_id || !d->namelen) continue;
            /* '.' and '..' are synthesized above; skip their on-disk copies. */
            if (d->namelen == 1 && d->name[0] == '.') continue;
            if (d->namelen == 2 && d->name[0] == '.' && d->name[1] == '.') continue;
            if (count < max)
            {
                ents[count].type = (d->type == AXFS_INODE_DIR) ? DT_DIR : DT_FILE;
                int n = 0;
                while (n <= MAX_NAME && n < d->namelen) { ents[count].name[n] = d->name[n]; n++; }
                ents[count].name[n > MAX_NAME ? MAX_NAME : n] = 0;
            }
            count++;
        }
    }
    return count;
}

/* Resolve the parent directory block + final component name of `relpath`. */
static uint64_t axfs_parent_dir(struct axfs_state *st, const char *relpath, char *name)
{
    const char *slash = 0;
    for (const char *q = relpath; *q; q++) if (*q == '/') slash = q;
    if (!slash)
    {
        size_t l = 0;
        while (relpath[l] && l < MAX_NAME) { name[l] = relpath[l]; l++; }
        name[l] = 0;
        return st->root_inode_block;
    }
    size_t dl = (size_t)(slash - relpath);
    if (dl > MAX_NAME) dl = MAX_NAME;
    char dpath[MAX_NAME + 1];
    for (size_t i = 0; i < dl; i++) dpath[i] = relpath[i];
    dpath[dl] = 0;
    uint64_t cur = st->root_inode_block;
    if (dl > 0)
    {
        const char *p = dpath;
        while (*p)
        {
            while (*p == '/') p++;
            if (!*p) break;
            const char *seg = p;
            while (*p && *p != '/') p++;
            size_t sl = (size_t)(p - seg);
            if (sl > MAX_NAME) sl = MAX_NAME;
            char comp[MAX_NAME + 1];
            for (size_t i = 0; i < sl; i++) comp[i] = seg[i];
            comp[sl] = 0;
            struct axfs_inode di;
            afs_read_inode(st, cur, &di);
            uint64_t next = 0;
            for (uint32_t e = 0; e < di.extent_count; e++)
            {
                uint8_t blk[AXFS_BLOCK_SIZE];
                afs_read_block(st, di.extents[e].physical_block, blk);
                for (int i = 0; i < 15; i++)
                {
                    struct axfs_dent *d = (struct axfs_dent *)(blk + i * sizeof(struct axfs_dent));
                    if (d->inode_id && d->namelen &&
                        __builtin_strncmp(d->name, comp, d->namelen) == 0 && d->namelen == sl)
                    { next = d->inode_id; break; }
                }
                if (next) break;
            }
            if (!next) return 0;
            cur = next;
        }
    }
    const char *base = slash + 1;
    size_t bl = __builtin_strlen(base);
    if (bl > MAX_NAME) bl = MAX_NAME;
    for (size_t i = 0; i < bl; i++) name[i] = base[i];
    name[bl] = 0;
    return cur;
}

static int axfs_create(struct vfs_super *sb, const char *relpath, int type)
{
    struct axfs_state *st = sb->priv;
    char name[MAX_NAME + 1];
    uint64_t parent = axfs_parent_dir(st, relpath, name);
    if (!parent) return -1;
    if (__builtin_strlen(name) == 0) return -1;

    uint64_t child = afs_alloc_block(st);
    struct axfs_inode ci;
    __builtin_memset(&ci, 0, sizeof(ci));
    ci.hdr.object_id = child;
    ci.hdr.type = AXFS_OBJ_INODE;
    ci.hdr.transaction_id = ++st->transaction_id;
    ci.inode_number = child;
    ci.in_type = (type == VFS_DIR) ? AXFS_INODE_DIR : AXFS_INODE_FILE;
    ci.size = 0;
    ci.extent_count = 0;
    /* ownership / permissions (user rank system) */
    {
        struct thread *t = sched_current();
        ci.uid  = t ? t->uid  : 0;
        ci.gid  = t ? t->gid  : 0;
        ci.permissions = (type == VFS_DIR) ? 0755 : 0644;
    }
    uint8_t cblk[AXFS_BLOCK_SIZE];
    __builtin_memcpy(cblk, &ci, sizeof(ci));
    afs_set_checksum(cblk);
    afs_write_block(st, child, cblk);

    afs_dir_add_entry(st, parent, name, child,
                      (type == VFS_DIR) ? AXFS_INODE_DIR : AXFS_INODE_FILE);
    return 0;
}

static int axfs_remove(struct vfs_super *sb, const char *relpath)
{
    struct axfs_state *st = sb->priv;
    char name[MAX_NAME + 1];
    uint64_t parent = axfs_parent_dir(st, relpath, name);
    if (!parent) return -1;

    /* find child inode block */
    struct axfs_inode pi;
    afs_read_inode(st, parent, &pi);
    uint64_t child = 0;
    for (uint32_t e = 0; e < pi.extent_count; e++)
    {
        uint8_t blk[AXFS_BLOCK_SIZE];
        afs_read_block(st, pi.extents[e].physical_block, blk);
        for (int i = 0; i < 15; i++)
        {
            struct axfs_dent *d = (struct axfs_dent *)(blk + i * sizeof(struct axfs_dent));
            if (d->inode_id && d->namelen &&
                __builtin_strncmp(d->name, name, d->namelen) == 0 && d->namelen == __builtin_strlen(name))
            { child = d->inode_id; break; }
        }
        if (child) break;
    }
    if (!child) return -1;

    afs_dir_remove_entry(st, parent, name);

    /* free the child inode + its data blocks */
    struct axfs_inode ci;
    afs_read_inode(st, child, &ci);
    for (uint32_t e = 0; e < ci.extent_count; e++)
        afs_free_block(st, ci.extents[e].physical_block);
    afs_free_block(st, child);
    return 0;
}

static void axfs_inode_free(struct vnode *n)
{
    if (!n) return;
    if (n->sb && n == n->sb->root)
        return; /* keep the cached root inode */
    kfree(n);
}

static struct vfs_fops g_axfs_ops = {
    .lookup = axfs_lookup,
    .read   = axfs_read,
    .list   = axfs_list,
    .create = axfs_create,
    .remove = axfs_remove,
    .write  = axfs_write,
    .mmap   = 0,
    .inode_free = axfs_inode_free,
};

int axiomefs_mount_part(int bus, int drive, int part, const char *mp)
{
    ide_probe(0, 0);
    struct block_dev *bd = ide_get_dev(bus, drive);
    if (!bd) return -1;
    return axiomefs_mount_block(bd, part, mp);
}

int axiomefs_mount_block(struct block_dev *bd, int part, const char *mp)
{
    if (!bd || !bd->present) return -1;
    uint32_t start = 0;
    if (part > 0)
    {
        struct partition parts[IDE_MAX_PARTS];
        int np = ide_read_partitions(bd, parts, IDE_MAX_PARTS);
        if (part < np) start = parts[part].start_lba;
        else return -1;
    }
    uint8_t blk[AXFS_BLOCK_SIZE];
    if (blk_read(bd, (uint64_t)start, AXFS_SECTORS_PER_BLOCK, blk) != 0)
        return -1;
    struct axfs_super *s = (struct axfs_super *)blk;
    if (__builtin_memcmp(s->magic, AXFS_MAGIC, 8) != 0)
    {
        if (bd->bus == 99)
            printk("axiomefs: bad magic on USB block (start %u)\n", start);
        else
            printk("axiomefs: bad magic at bus %d drive %d\n", bd->bus, bd->drive);
        return -1;
    }
    struct axfs_state *st = kmalloc(sizeof(*st));
    if (!st) return -1;
    __builtin_memset(st, 0, sizeof(*st));
    st->bd = bd;
    st->part_start = start;
    st->total_blocks = s->total_blocks;
    st->root_inode_block = s->root_inode;
    st->free_bitmap_block = s->free_bitmap_block;
    st->transaction_id = s->transaction_id + 1;
    struct vfs_super *sb = vfs_mount(mp, FS_AXIOMEFS, &g_axfs_ops, st);
    if (!sb) { kfree(st); return -1; }
    st->root = axfs_make_vnode(sb, st, st->root_inode_block);
    if (!st->root) { kfree(st); return -1; }
    sb->root = st->root;
    printk("axiomefs: mounted '%s' root=%lu blocks=%lu txn=%lu\n",
           mp, (unsigned long)st->root_inode_block, (unsigned long)st->total_blocks,
           (unsigned long)st->transaction_id);
    return 0;
}
