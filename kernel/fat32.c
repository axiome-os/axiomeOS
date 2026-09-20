#include "fat32.h"
#include "vfs.h"
#include "ide.h"
#include "xhci.h"
#include "io.h"
#include "slab.h"
#include "printk.h"
#include "string.h"
#include <stddef.h>

/* ---- FAT32 on-disk state (parsed BPB) ---- */
struct fat32_state {
    struct block_dev *bd;
    uint32_t part_start;      /* LBA of partition start */
    uint32_t byts_per_sec;
    uint32_t sec_per_clus;
    uint32_t rsvd_sec_cnt;
    uint32_t num_fats;
    uint32_t fatsz32;
    uint32_t root_clus;
    uint32_t fat_lba;         /* first FAT sector (relative to partition) */
    uint32_t data_start;      /* first data sector (relative to partition) */
    struct vnode *root;       /* cached root inode */
    /* Mount-time ownership/mode defaults (POSIX compatibility shim). */
    uint32_t mount_uid;
    uint32_t mount_gid;
    uint32_t mount_umask;
};

/* ---- tiny helpers ---- */
static int f_stricmp(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca |= 0x20;
        if (cb >= 'A' && cb <= 'Z') cb |= 0x20;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Read `count` sectors at partition-relative LBA. */
static int fat_read_sectors(struct fat32_state *st, uint32_t lba, uint32_t count, void *buf)
{
    return blk_read(st->bd, (uint64_t)st->part_start + lba, count, buf);
}

/* Get the FAT entry for a cluster (next cluster or end marker). */
static uint32_t fat_next(struct fat32_state *st, uint32_t cluster)
{
    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint32_t fat_sec = st->fat_lba + (uint32_t)(fat_offset / st->byts_per_sec);
    uint32_t off = (uint32_t)(fat_offset % st->byts_per_sec);
    uint8_t sec[512];
    if (fat_read_sectors(st, fat_sec, 1, sec) != 0)
        return 0x0FFFFFF7;
    uint32_t val;
    __builtin_memcpy(&val, sec + off, 4);
    return val & 0x0FFFFFFF;
}

/* Build the 8.3 short name from the 11-byte directory field. */
static void fat_short_name(const uint8_t *de, char *out)
{
    int j = 0;
    uint8_t c0 = de[0];
    if (c0 == 0x05) c0 = 0xE5; /* Kanji leading byte */
    int i = 0;
    for (; i < 8; i++) { uint8_t c = de[i]; if (c == ' ') break; out[j++] = (char)c; }
    if (de[8] != ' ')
    {
        out[j++] = '.';
        for (i = 0; i < 3; i++) { uint8_t c = de[8 + i]; if (c == ' ') break; out[j++] = (char)c; }
    }
    out[j] = 0;
}

/* Parse LFN entry (attr 0x0F) into the accumulating long-name buffer.
   Stops at a UTF-16 NUL or 0xFFFF padding so trailing garbage is excluded. */
static void fat_lfn_accumulate(const uint8_t *de, char *lfn, int *lfn_len)
{
    const uint16_t *a = (const uint16_t *)(de + 1);  /* 5 chars */
    const uint16_t *b = (const uint16_t *)(de + 14); /* 6 chars */
    const uint16_t *c = (const uint16_t *)(de + 28); /* 2 chars */
    for (int k = 0; k < 5 && *lfn_len < 255; k++)
    {
        uint16_t ch = a[k];
        if (ch == 0 || ch == 0xFFFF) return;
        lfn[(*lfn_len)++] = (char)(ch & 0xFF);
    }
    for (int k = 0; k < 6 && *lfn_len < 255; k++)
    {
        uint16_t ch = b[k];
        if (ch == 0 || ch == 0xFFFF) return;
        lfn[(*lfn_len)++] = (char)(ch & 0xFF);
    }
    for (int k = 0; k < 2 && *lfn_len < 255; k++)
    {
        uint16_t ch = c[k];
        if (ch == 0 || ch == 0xFFFF) return;
        lfn[(*lfn_len)++] = (char)(ch & 0xFF);
    }
}

struct fat_dirent {
    char name[MAX_NAME + 1];
    int is_dir;
    uint32_t first_cluster;
    uint32_t size;
};

/* Find a named entry inside the directory beginning at `dir_first`.
   Returns 1 on match (fills `out`), 0 if not found, -1 on I/O error. */
static int fat_find_in_dir(struct fat32_state *st, uint32_t dir_first,
                           const char *name, struct fat_dirent *out)
{
    uint32_t clus = dir_first;
    uint8_t sec[512];
    char lfn[256];
    int lfn_len = 0;

    while (clus >= 2 && clus < 0x0FFFFFF8)
    {
        uint64_t lba = (uint64_t)st->data_start + (uint64_t)(clus - 2) * st->sec_per_clus;
        for (uint32_t s = 0; s < st->sec_per_clus; s++)
        {
            if (fat_read_sectors(st, (uint32_t)(lba + s), 1, sec) != 0)
                return -1;
            for (int e = 0; e < 16; e++)
            {
                uint8_t *de = sec + e * 32;
                uint8_t attr = de[11];
                if (de[0] == 0x00)
                    return 0;                 /* end of directory */
                if (de[0] == 0xE5) { lfn_len = 0; continue; } /* deleted */
                if (attr == 0x0F) { fat_lfn_accumulate(de, lfn, &lfn_len); continue; }
                if (de[0] == '.') { lfn_len = 0; continue; }   /* . and .. */
                if (attr & 0x08) { lfn_len = 0; continue; }    /* volume label */

                char sname[13];
                fat_short_name(de, sname);
                const char *disp = sname;
                if (lfn_len > 0) { lfn[lfn_len] = 0; disp = lfn; }

                if (f_stricmp(disp, name) == 0)
                {
                    out->is_dir = (attr & 0x10) != 0;
                    __builtin_memcpy(&out->size, de + 28, 4);
                    uint16_t lo, hi;
                    __builtin_memcpy(&lo, de + 26, 2);
                    __builtin_memcpy(&hi, de + 20, 2);
                    out->first_cluster = ((uint32_t)hi << 16) | lo;
                    int n = 0;
                    while (n <= MAX_NAME && disp[n]) { out->name[n] = disp[n]; n++; }
                    out->name[n > MAX_NAME ? MAX_NAME : n] = 0;
                    return 1;
                }
                lfn_len = 0;
            }
        }
        clus = fat_next(st, clus);
    }
    return 0;
}

/* Read `len` bytes starting at byte offset `off` from a cluster chain. */
static size_t fat_read_chain(struct fat32_state *st, uint32_t first,
                             size_t off, void *buf, size_t len)
{
    uint32_t clus = first;
    size_t clus_bytes = (size_t)st->sec_per_clus * st->byts_per_sec;
    uint64_t skip = off / clus_bytes;
    for (uint64_t i = 0; i < skip; i++)
    {
        clus = fat_next(st, clus);
        if (clus < 2 || clus >= 0x0FFFFFF8) return 0;
    }
    size_t done = 0;
    size_t in_off = off % clus_bytes;
    uint8_t *p = (uint8_t *)buf;

    while (done < len && clus >= 2 && clus < 0x0FFFFFF8)
    {
        uint64_t lba = (uint64_t)st->data_start + (uint64_t)(clus - 2) * st->sec_per_clus;
        uint8_t *tmp = kmalloc(clus_bytes);
        if (!tmp) return done;
        if (fat_read_sectors(st, (uint32_t)lba, st->sec_per_clus, tmp) != 0)
        {
            kfree(tmp);
            return done;
        }
        size_t avail = clus_bytes - in_off;
        size_t chunk = len - done;
        if (chunk > avail) chunk = avail;
        for (size_t i = 0; i < chunk; i++) p[done + i] = tmp[in_off + i];
        done += chunk;
        in_off = 0;
        kfree(tmp);
        if (done >= len) break;
        clus = fat_next(st, clus);
    }
    return done;
}

/* ---- VFS operations ---- */
static struct vnode *fat_make_vnode(struct vfs_super *sb, const struct fat_dirent *fe)
{
    struct vnode *n = kmalloc(sizeof(struct vnode));
    if (!n) return 0;
    memset(n, 0, sizeof(*n));
    n->sb = sb;
    int n2 = 0;
    while (n2 <= MAX_NAME && fe->name[n2]) { n->name[n2] = fe->name[n2]; n2++; }
    n->name[n2 > MAX_NAME ? MAX_NAME : n2] = 0;
    n->type = fe->is_dir ? VFS_DIR : VFS_FILE;
    n->size = fe->size;
    n->priv = (void *)(uintptr_t)fe->first_cluster;
    /* FAT32 has no native POSIX ownership; inherit the mount defaults and
       derive the mode from the umask (dirs get execute, files do not). */
    struct fat32_state *st = sb->priv;
    n->v_uid = st ? st->mount_uid : 0;
    n->v_gid = st ? st->mount_gid : 0;
    uint32_t umask = st ? st->mount_umask : 022;
    n->v_mode = (fe->is_dir ? (0777 & ~umask) : (0666 & ~umask));
    return n;
}

static struct vnode *fat32_lookup(struct vfs_super *sb, struct vnode *dir,
                                  const char *relpath)
{
    struct fat32_state *st = sb->priv;
    uint32_t cur = (uint32_t)(uintptr_t)dir->priv;
    const char *p = relpath;
    if (p[0] == '/') p++;
    char comp[MAX_NAME + 1];

    while (*p)
    {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t sl = (size_t)(p - seg);
        if (sl > MAX_NAME) sl = MAX_NAME;
        for (size_t i = 0; i < sl; i++) comp[i] = seg[i];
        comp[sl] = 0;

        struct fat_dirent fe;
        int r = fat_find_in_dir(st, cur, comp, &fe);
        if (r != 1) return 0;

        if (*p == 0)
            return fat_make_vnode(sb, &fe);
        cur = fe.first_cluster;
    }
    /* Trailing slash or empty tail: return the directory itself. */
    struct fat_dirent fe;
    if (fat_find_in_dir(st, cur, ".", &fe) == 1)
        return fat_make_vnode(sb, &fe);
    return 0;
}

static size_t fat32_read(struct vfs_super *sb, struct vnode *n, size_t off,
                         void *buf, size_t len)
{
    struct fat32_state *st = sb->priv;
    uint32_t first = (uint32_t)(uintptr_t)n->priv;
    if (first < 2) return 0;
    if (off >= n->size) return 0;
    size_t avail = n->size - off;
    if (len > avail) len = avail;
    return fat_read_chain(st, first, off, buf, len);
}

static int fat32_list(struct vfs_super *sb, struct vnode *dir,
                      struct vfs_dirent *ents, int max)
{
    struct fat32_state *st = sb->priv;
    uint32_t cur = (uint32_t)(uintptr_t)dir->priv;
    int count = 0;
    if (count < max) { ents[count].type = DT_DIR; for (int k=0;k<=MAX_NAME;k++) ents[count].name[k] = (k==0?'.':'\0'); }
    count++;
    if (count < max) { ents[count].type = DT_DIR; ents[count].name[0]='.'; ents[count].name[1]='.'; ents[count].name[2]=0; }
    count++;

    uint32_t clus = cur;
    uint8_t sec[512];
    char lfn[256];
    int lfn_len = 0;

    while (clus >= 2 && clus < 0x0FFFFFF8)
    {
        uint64_t lba = (uint64_t)st->data_start + (uint64_t)(clus - 2) * st->sec_per_clus;
        for (uint32_t s = 0; s < st->sec_per_clus; s++)
        {
            if (fat_read_sectors(st, (uint32_t)(lba + s), 1, sec) != 0)
                return count;
            for (int e = 0; e < 16; e++)
            {
                uint8_t *de = sec + e * 32;
                uint8_t attr = de[11];
                if (de[0] == 0x00) return count;
                if (de[0] == 0xE5) { lfn_len = 0; continue; }
                if (attr == 0x0F) { fat_lfn_accumulate(de, lfn, &lfn_len); continue; }
                if (de[0] == '.') { lfn_len = 0; continue; }
                if (attr & 0x08) { lfn_len = 0; continue; }

                char sname[13];
                fat_short_name(de, sname);
                const char *disp = sname;
                if (lfn_len > 0) { lfn[lfn_len] = 0; disp = lfn; }
                if (count < max)
                {
                    ents[count].type = (attr & 0x10) ? DT_DIR : DT_FILE;
                    int n = 0;
                    while (n <= MAX_NAME && disp[n]) { ents[count].name[n] = disp[n]; n++; }
                    ents[count].name[n > MAX_NAME ? MAX_NAME : n] = 0;
                }
                count++;
                lfn_len = 0;
            }
        }
        clus = fat_next(st, clus);
    }
    return count;
}

static void fat32_inode_free(struct vnode *n)
{
    if (!n) return;
    if (n->sb && n == n->sb->root)
        return; /* keep the cached root inode */
    kfree(n);
}

static struct vfs_fops g_fat32_ops = {
    .lookup = fat32_lookup,
    .read   = fat32_read,
    .list   = fat32_list,
    .create = 0,
    .remove = 0,
    .write  = 0,
    .mmap   = 0,
    .inode_free = fat32_inode_free,
};

/* Parse a FAT32 BPB; returns 0 on success. */
static int fat32_parse_bpb(struct fat32_state *st, const uint8_t *bpb)
{
    st->byts_per_sec = (uint32_t)bpb[11] | ((uint32_t)bpb[12] << 8);
    st->sec_per_clus = bpb[13];
    st->rsvd_sec_cnt = (uint32_t)bpb[14] | ((uint32_t)bpb[15] << 8);
    st->num_fats = bpb[16];
    st->fatsz32 = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
                  ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
    st->root_clus = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                    ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);
    st->fat_lba = st->rsvd_sec_cnt;
    st->data_start = st->rsvd_sec_cnt + st->num_fats * st->fatsz32;

    /* FAT32 sanity checks: no 16-bit FAT, root dir entries == 0. */
    uint16_t fatsz16 = (uint16_t)((uint32_t)bpb[22] | ((uint32_t)bpb[23] << 8));
    uint16_t rootent = (uint16_t)((uint32_t)bpb[17] | ((uint32_t)bpb[18] << 8));
    if (fatsz16 != 0 || rootent != 0)
        return -1;
    if (st->byts_per_sec == 0 || st->sec_per_clus == 0 || st->fatsz32 == 0)
        return -1;
    if (st->root_clus < 2)
        return -1;
    return 0;
}

/* Try to mount a FAT32 volume at `part_start` on a device at `mp`. Returns 1 on success. */
static int fat32_try_mount(struct block_dev *bd, uint32_t part_start, const char *mp)
{
    uint8_t bpb[512];
    if (blk_read(bd, part_start, 1, bpb) != 0)
    {
        printk("FAT32: failed to read boot sector at LBA %lu\n",
               (unsigned long)part_start);
        return 0;
    }
    struct fat32_state *st = kmalloc(sizeof(*st));
    if (!st) return 0;
    memset(st, 0, sizeof(*st));
    st->bd = bd;
    st->part_start = part_start;
    /* Default POSIX ownership/mode: uid 0, gid 0, umask 022 -> dirs 0755, files 0644. */
    st->mount_uid = 0;
    st->mount_gid = 0;
    st->mount_umask = 022;
    if (fat32_parse_bpb(st, bpb) != 0)
    {
        kfree(st);
        return 0;
    }
    struct vfs_super *sb = vfs_mount(mp, FS_FAT32, &g_fat32_ops, st);
    if (!sb) { kfree(st); return 0; }
    struct vnode *root = kmalloc(sizeof(struct vnode));
    if (!root) { kfree(st); return 0; }
    memset(root, 0, sizeof(*root));
    root->sb = sb;
    root->name[0] = '/';
    root->name[1] = 0;
    root->type = VFS_DIR;
    root->size = 0;
    root->priv = (void *)(uintptr_t)st->root_clus;
    sb->root = root;
    st->root = root;
    printk("FAT32: mounted at LBA %lu (root cluster %lu) on %s\n",
           (unsigned long)part_start, (unsigned long)st->root_clus, mp);
    return 1;
}

/* Mount the FAT32 partition `part` (0-based index into the device's MBR
   partition table; if the device has no partition table, part 0 means the
   whole device is a FAT32 volume) of IDE (bus,drive) at `mp`.
   Returns 0 on success, -1 on failure. */
int fat32_mount_part(int bus, int drive, int part, const char *mp)
{
    struct block_dev *bd = ide_get_dev(bus, drive);
    if (!bd)
        return -1;
    struct partition parts[IDE_MAX_PARTS];
    int np = ide_read_partitions(bd, parts, IDE_MAX_PARTS);
    if (np > 0)
    {
        if (part >= 0 && part < np) {
            uint8_t t = parts[part].type;
            if (t == 0x0B || t == 0x0C || t == 0x0E || t == 0xEF) {
                if (fat32_try_mount(bd, parts[part].start_lba, mp))
                    return 0;
                return -1;
            }
        }
        return -1;
    }
    /* No partition table: treat the whole device as a FAT32 volume. */
    if (part == 0)
        return fat32_try_mount(bd, 0, mp) ? 0 : -1;
    return -1;
}

static int fat32_is_fat_type(uint8_t t)
{
    return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C ||
           t == 0x0E || t == 0xEF;
}

void fat32_automount(void)
{
    for (int bus = 0; bus < 2; bus++)
    {
        for (int drive = 0; drive < 2; drive++)
        {
            struct block_dev *bd = ide_get_dev(bus, drive);
            if (!bd)
                continue;
            struct partition parts[IDE_MAX_PARTS];
            int np = ide_read_partitions(bd, parts, IDE_MAX_PARTS);
            for (int p = 0; p < np; p++)
            {
                if (!fat32_is_fat_type(parts[p].type))
                    continue;
                if (fat32_try_mount(bd, parts[p].start_lba, "/boot"))
                    return; /* mounted the first FAT32 partition */
            }
            /* Superfloppy fallback: the whole device is a FAT32 volume. */
            if (fat32_try_mount(bd, 0, "/boot"))
                return;
        }
    }
    /* Try USB mass-storage (xHCI) devices as a fallback / primary when
       booting from a flash drive. The xHCI driver exposes each flash drive
       as a block_dev with BOT/SCSI translation, so the same FAT32 logic
       applies (MBR parse + superfloppy). */
    for (int i = 0; i < 16; i++)
    {
        struct block_dev *bd = xhci_get_block_dev(i);
        if (!bd)
            break;
        struct partition parts[IDE_MAX_PARTS];
        int np = ide_read_partitions(bd, parts, IDE_MAX_PARTS);
        for (int p = 0; p < np; p++)
        {
            if (!fat32_is_fat_type(parts[p].type))
                continue;
            if (fat32_try_mount(bd, parts[p].start_lba, "/boot"))
                return;
        }
        if (fat32_try_mount(bd, 0, "/boot"))
            return;
    }
    printk("FAT32: no FAT32 volume found\n");
}
