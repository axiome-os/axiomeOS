#include "ide.h"
#include "io.h"
#include "printk.h"

#define IDE_PRIMARY_BASE   0x1F0
#define IDE_PRIMARY_CTRL   0x3F6
#define IDE_SECONDARY_BASE 0x170
#define IDE_SECONDARY_CTRL 0x376

#define REG_DATA    0
#define REG_ERROR   1
#define REG_SECCNT  2
#define REG_LBALO   3
#define REG_LBAMID  4
#define REG_LBAHI   5
#define REG_DRIVE   6
#define REG_STATUS  7  /* command block (write = command) */
#define REG_CMD     7

/* Status register bits. */
#define ST_BSY 0x80
#define ST_DRDY 0x40
#define ST_DRQ 0x08
#define ST_ERR 0x01

#define CMD_READ_SECTORS 0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_IDENTIFY     0xEC

static uint16_t base_port(int bus) { return bus == 0 ? IDE_PRIMARY_BASE : IDE_SECONDARY_BASE; }
static uint16_t ctrl_port(int bus) { return bus == 0 ? IDE_PRIMARY_CTRL : IDE_SECONDARY_CTRL; }

static inline void ata_delay_400ns(uint16_t ctrl)
{
    /* Reading the alternate status 5 times yields ~400ns on most HW. */
    for (int i = 0; i < 5; i++) inb(ctrl);
}

static int wait_ready(uint16_t base, uint16_t ctrl)
{
    ata_delay_400ns(ctrl);
    uint8_t s;
    uint64_t timeout = 31000000;
    do {
        s = inb(base + REG_STATUS);
        if (timeout-- == 0) return -1;
    } while (s & ST_BSY);
    return (s & ST_ERR) ? -1 : 0;
}

static int wait_drq(uint16_t base, uint16_t ctrl)
{
    uint8_t s;
    uint64_t timeout = 30000000;
    do {
        s = inb(base + REG_STATUS);
        if (s & ST_ERR) return -1;
        if (timeout-- == 0) return -1;
    } while ((s & ST_BSY) || !(s & ST_DRQ));
    return 0;
}

int blk_read(struct block_dev *bd, uint64_t lba, uint32_t count, void *buf)
{
    if (!bd || !bd->present || count == 0)
        return -1;
    if (bd->read_fn)
        return bd->read_fn(bd, lba, count, buf);
    uint16_t base = base_port(bd->bus);
    uint16_t ctrl = ctrl_port(bd->bus);
    uint8_t *p = (uint8_t *)buf;

    if (lba > 0xFFFFFFFULL)
        return -1; /* 28-bit LBA only */

    for (uint32_t done = 0; done < count; )
    {
        uint32_t chunk = count - done;
        if (chunk > 255) chunk = 255;
        chunk = 1; /* single-sector transfers (reliable on emulated PIO) */

        if (wait_ready(base, ctrl) != 0)
            return -1;

        uint8_t dh = 0xE0 | (bd->drive << 4) | ((lba >> 24) & 0x0F);
        outb(base + REG_DRIVE, dh);
        ata_delay_400ns(ctrl); /* settle after drive select */

        outb(base + REG_SECCNT, (uint8_t)chunk);
        outb(base + REG_LBALO, (uint8_t)(lba & 0xFF));
        outb(base + REG_LBAMID, (uint8_t)((lba >> 8) & 0xFF));
        outb(base + REG_LBAHI, (uint8_t)((lba >> 16) & 0xFF));
        outb(base + REG_CMD, CMD_READ_SECTORS);

        for (uint32_t i = 0; i < chunk; i++)
        {
            if (wait_drq(base, ctrl) != 0)
                return -1;
            insw(base + REG_DATA, p, 256);
            p += 512;
            done++;
        }
        lba += chunk;
    }
    return 0;
}

static int identify_drive(int bus, int drive, uint64_t *total_sectors)
{
    uint16_t base = base_port(bus);
    uint16_t ctrl = ctrl_port(bus);

    if (wait_ready(base, ctrl) != 0)
        return -1;

    uint8_t dh = 0xE0 | (drive << 4);
    outb(base + REG_DRIVE, dh);
    outb(base + REG_CMD, CMD_IDENTIFY);

    /* If status stays 0, no device is attached (floating bus). */
    ata_delay_400ns(ctrl);
    uint8_t s = inb(base + REG_STATUS);
    if (s == 0)
        return -1;

    if (wait_drq(base, ctrl) != 0)
        return -1;

    uint16_t id[256];
    insw(base + REG_DATA, id, 256);

    /* Word 83 bit 10 => 48-bit LBA supported. */
    int lba48 = (id[83] & (1u << 10)) != 0;
    if (lba48)
        *total_sectors = ((uint64_t)id[103] << 48) |
                         ((uint64_t)id[102] << 32) |
                         ((uint64_t)id[101] << 16) |
                         (uint64_t)id[100];
    else
        /* Word 60/61: total user addressable sectors (28-bit). */
        *total_sectors = ((uint64_t)id[61] << 16) | (uint64_t)id[60];
    return 0;
}

/* Persistent table of probed devices. Mount handlers must NOT store a
   pointer into a caller-local probe array (it goes out of scope); use
   ide_get_dev() to obtain a pointer into this table instead. */
static struct block_dev g_devs[4];
static int g_ndevs;

int ide_probe(struct block_dev *devs, int max)
{
    int found = 0;
    g_ndevs = 0;
    static const int buses[2] = { 0, 1 };
    for (int b = 0; b < 2; b++)
    {
        for (int d = 0; d < 2; d++)
        {
            if (found >= 4)
                return found;
            struct block_dev *bd = &g_devs[found];
            bd->bus = buses[b];
            bd->drive = d;
            bd->present = 0;
            bd->total_sectors = 0;
            uint64_t ts = 0;
            if (identify_drive(b, d, &ts) == 0 && ts > 0)
            {
                bd->present = 1;
                bd->total_sectors = ts;
                printk("IDE: bus=%d drive=%d present sectors=%lu\n",
                       b, d, (unsigned long)ts);
                found++;
                g_ndevs = found;
            }
        }
    }
    /* Also mirror into the caller's array for any legacy scanning. */
    if (devs)
    {
        for (int i = 0; i < found && i < max; i++)
            devs[i] = g_devs[i];
    }
    if (found == 0)
        printk("IDE: no drives detected\n");
    return found;
}

struct block_dev *ide_get_dev(int bus, int drive)
{
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i].bus == bus && g_devs[i].drive == drive)
            return &g_devs[i];
    return 0;
}

int ide_read_partitions(struct block_dev *bd, struct partition *parts, int max)
{
    if (!bd || !bd->present)
        return 0;
    uint8_t mbr[512];
    if (blk_read(bd, 0, 1, mbr) != 0)
        return 0;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
    {
        printk("IDE: MBR signature missing\n");
        return 0;
    }
    int n = 0;
    for (int i = 0; i < 4 && n < max; i++)
    {
        const uint8_t *pe = mbr + 446 + i * 16;
        uint8_t type = pe[4];
        if (type == 0)
            continue;
        uint32_t start = (uint32_t)pe[8] | ((uint32_t)pe[9] << 8) |
                         ((uint32_t)pe[10] << 16) | ((uint32_t)pe[11] << 24);
        uint32_t num = (uint32_t)pe[12] | ((uint32_t)pe[13] << 8) |
                       ((uint32_t)pe[14] << 16) | ((uint32_t)pe[15] << 24);
        parts[n].type = type;
        parts[n].start_lba = start;
        parts[n].num_sectors = num;
        n++;
    }
    return n;
}

int blk_write(struct block_dev *bd, uint64_t lba, uint32_t count, const void *buf)
{
    if (!bd || !bd->present || count == 0)
        return -1;
    if (bd->write_fn)
        return bd->write_fn(bd, lba, count, buf);
    uint16_t base = base_port(bd->bus);
    uint16_t ctrl = ctrl_port(bd->bus);
    const uint8_t *p = (const uint8_t *)buf;

    if (lba > 0xFFFFFFFULL)
        return -1; /* 28-bit LBA only */

    for (uint32_t done = 0; done < count; )
    {
        uint32_t chunk = count - done;
        if (chunk > 255) chunk = 255;

        if (wait_ready(base, ctrl) != 0)
            return -1;

        uint8_t dh = 0xE0 | (bd->drive << 4) | ((lba >> 24) & 0x0F);
        outb(base + REG_DRIVE, dh);
        outb(base + REG_SECCNT, (uint8_t)chunk);
        outb(base + REG_LBALO, (uint8_t)(lba & 0xFF));
        outb(base + REG_LBAMID, (uint8_t)((lba >> 8) & 0xFF));
        outb(base + REG_LBAHI, (uint8_t)((lba >> 16) & 0xFF));
        outb(base + REG_CMD, CMD_WRITE_SECTORS);

        for (uint32_t i = 0; i < chunk; i++)
        {
            uint8_t s;
            uint64_t write_timeout = 30000000;
            do {
                s = inb(base + REG_STATUS);
                if (s & ST_ERR) return -1;
                if (write_timeout-- == 0) return -1;
            } while ((s & ST_BSY) || !(s & ST_DRQ));
            outsw(base + REG_DATA, p, 256);
            p += 512;
            done++;
        }
        lba += chunk;
    }
    return 0;
}
