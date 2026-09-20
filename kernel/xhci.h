#ifndef AXIOME_XHCI_H
#define AXIOME_XHCI_H

#include <stdint.h>

struct pci_device;
struct block_dev;

int xhci_probe(struct pci_device *pdev);
void xhci_poll(void);

/* USB mass-storage block device helpers (for fat32_automount). */
struct block_dev *xhci_get_block_dev(int idx);
int xhci_block_count(void);
int xhci_storage_read(uint64_t lba, uint32_t count, void *buf);

#endif
