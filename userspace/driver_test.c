#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "syscall.h"

int main(void)
{
    printf("driver_test: starting\n");

    /* 12.2 PCI devices are printed by pci_init at boot (check kernel log). */

    /* 12.8 /dev listing */
    printf("\n--- /dev listing ---\n");
    struct vfs_dirent ents[64];
    int n = readdir("/dev", ents, 64);
    for (int i = 0; i < n; i++)
        printf("  /dev/%s%s\n", ents[i].name,
               ents[i].type == DT_DIR ? "/" : "");

    /* 12.8 /dev/zero: read 32 bytes of zeros */
    printf("\n--- /dev/zero test ---\n");
    int zfd = open("/dev/zero", 0x0000);  /* O_RDONLY = 0 */
    if (zfd >= 0)
    {
        char buf[32];
        long nr = read(zfd, buf, sizeof(buf));
        int all_zero = 1;
        for (int i = 0; i < nr; i++)
            if (buf[i] != 0) all_zero = 0;
        printf("read %ld bytes, all_zero=%s\n", nr, all_zero ? "YES" : "NO");
        close(zfd);
    }
    else
        printf("/dev/zero: open failed\n");

    /* 12.8 /dev/ide0: read the first sector (MBR) */
    printf("\n--- /dev/ide0 test (sector 0) ---\n");
    int id = open("/dev/ide0", 0x0000);
    if (id >= 0)
    {
        char buf[512];
        long nr = read(id, buf, 512);
        printf("read %ld bytes from /dev/ide0\n", nr);
        if (nr >= 512)
        {
            /* MBR signature at byte 510-511 should be 0x55 0xAA */
            printf("MBR sig: %02x %02x (expect 55 aa)\n",
                   (uint8_t)buf[510], (uint8_t)buf[511]);
        }
        close(id);
    }
    else
        printf("/dev/ide0: open failed\n");

    /* 12.9 Hotplug re-scan (idempotent) */
    printf("\n--- driver_rescan (12.9) ---\n");
    driver_rescan();
    printf("driver_rescan done\n");

    printf("\ndriver_test: done\n");
    sys_exit(0);
    return 0;
}
