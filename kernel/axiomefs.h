#ifndef AXIOME_AXIOMEFS_H
#define AXIOME_AXIOMEFS_H

#include <stdint.h>
#include <stddef.h>

/* Minimal-but-faithful implementation of the axiomefs v1 spec
   (see ./axiomefs.md). Captures the core idioms:
     - 4096-byte, little-endian blocks
     - dual superblocks (primary + checkpoint backup) for crash recovery
     - common object_header on every object
     - extents (not direct block lists) for file data
     - Copy-on-Write block allocation (writes never overwrite live blocks)
     - 64-bit checksums in every object header
     - a free-space bitmap
   B+Trees are simplified to linear tables; snapshots/compression/encryption
   are omitted (noted as future work). */

#define AXFS_BLOCK_SIZE   4096
#define AXFS_MAGIC        "AXIOMEFS"

/* object_header.type */
#define AXFS_OBJ_SUPER    1
#define AXFS_OBJ_INODE    2

/* inode.in_type */
#define AXFS_INODE_FILE   1
#define AXFS_INODE_DIR    2

#define AXFS_MAX_EXTENTS  160

struct axfs_obj_hdr {
    uint64_t object_id;
    uint64_t transaction_id;
    uint32_t type;
    uint32_t flags;
    uint64_t checksum;
} __attribute__((packed));

struct axfs_extent {
    uint64_t physical_block;
    uint64_t block_count;   /* usually 1 in this minimal impl */
    uint32_t reference_count;
    uint32_t _pad;
} __attribute__((packed));

struct axfs_super {
    struct axfs_obj_hdr hdr;
    char     magic[8];
    uint64_t block_size;
    uint64_t total_blocks;
    uint64_t root_inode;       /* block number of the root dir inode */
    uint64_t object_map;       /* reserved (APFS-like object map) */
    uint64_t checkpoint_tree;  /* reserved */
    uint64_t transaction_id;
    uint64_t free_bitmap_block;
    uint64_t features;
    uint64_t compat;
    uint64_t ro_compat;
    uint64_t incompat;
    uint8_t  reserved[3968];
} __attribute__((packed));

struct axfs_inode {
    struct axfs_obj_hdr hdr;
    uint64_t inode_number;
    uint64_t size;
    uint64_t created;
    uint64_t modified;
    uint64_t permissions;
    uint32_t uid;
    uint32_t gid;
    uint32_t in_type;
    uint32_t extent_count;
    struct axfs_extent extents[AXFS_MAX_EXTENTS];
    uint8_t  reserved[168];
} __attribute__((packed));

/* A directory is just a file whose data blocks hold these entries. */
struct axfs_dent {
    uint32_t inode_id;   /* block number of the child inode */
    uint8_t  type;       /* AXFS_INODE_FILE / AXFS_INODE_DIR */
    uint8_t  namelen;
    uint16_t _pad;
    char     name[256];
} __attribute__((packed));

/* Parse/format an axiomefs volume on IDE (bus,drive), partition `part`
   (0 = whole device / superfloppy), and mount it at `mp`.
   Returns 0 on success, -1 on failure. */
int axiomefs_mount_part(int bus, int drive, int part, const char *mp);

/* Mount axiomefs from a generic block_dev (e.g. USB MSC). */
struct block_dev;
int axiomefs_mount_block(struct block_dev *bd, int part, const char *mp);

#endif
