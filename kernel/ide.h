#ifndef AXIOME_IDE_H
#define AXIOME_IDE_H

#include <stdint.h>
#include <stddef.h>

/* A simple block device backed by a legacy ATA (PATA) drive accessed via PIO,
   or by a USB mass-storage device (when ops are set). */
struct block_dev {
    int bus;             /* 0 = primary (0x1F0), 1 = secondary (0x170) */
    int drive;           /* 0 = master, 1 = slave */
    uint64_t total_sectors;
    int present;
    /* Optional override for non-IDE transports (USB MSC). When non-NULL,
       blk_read/blk_write dispatch through these instead of ATA PIO. */
    int (*read_fn)(struct block_dev *bd, uint64_t lba, uint32_t count, void *buf);
    int (*write_fn)(struct block_dev *bd, uint64_t lba, uint32_t count, const void *buf);
    void *priv;
};

/* One MBR partition entry of interest. */
struct partition {
    uint8_t type;
    uint32_t start_lba;
    uint32_t num_sectors;
};

#define IDE_MAX_PARTS 4

/* Detect attached PATA drives on the primary/secondary buses.
   Fills the provided array (size 4: primary-master, primary-slave,
   secondary-master, secondary-slave). Returns number of present drives. */
int ide_probe(struct block_dev *devs, int max);

/* Look up a previously probed device by (bus, drive). Returns a pointer into
   a persistent table (safe to store); NULL if not present. */
struct block_dev *ide_get_dev(int bus, int drive);

/* Read `count` 512-byte sectors starting at LBA into buf. 0 on success. */
int blk_read(struct block_dev *bd, uint64_t lba, uint32_t count, void *buf);

/* Write `count` 512-byte sectors from buf starting at LBA. 0 on success. */
int blk_write(struct block_dev *bd, uint64_t lba, uint32_t count, const void *buf);

/* Parse the MBR of `bd`, returning up to `max` partitions. Returns count. */
int ide_read_partitions(struct block_dev *bd, struct partition *parts, int max);

#endif
