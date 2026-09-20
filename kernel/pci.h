#ifndef AXIOME_PCI_H
#define AXIOME_PCI_H

#include <stdint.h>

#define PCI_ADDR_PORT 0xCF8
#define PCI_DATA_PORT 0xCFC

struct driver;   /* driver.h — forward-declared to avoid a circular include */

struct pci_device {
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t hdr_type;
    uint8_t irq;
    uint32_t bar[6];
    struct pci_device *next;
    /* Driver that claimed this function, or NULL. Set by driver_probe_pci()
       on the first successful match so later probe passes (module loads,
       rescans) skip it instead of re-running hardware init. Cleared by
       pci_init(), which rebuilds the list from scratch. */
    struct driver *owner;
};

/* Read a 32-bit configuration dword for (bus,dev,func) at offset `off`. */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);

/* Write a 32-bit configuration dword. */
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);

/* Read a 16-bit configuration halfword. */
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);

/* Write a 16-bit configuration halfword. */
void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t val);

/* Read a 8-bit configuration byte. */
uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);

/* Write a 8-bit configuration byte. */
void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint8_t val);

/* Enumerate the PCI bus(es) and print discovered devices. */
void pci_init(void);

/* Walk the discovered device list (NULL-terminated). */
struct pci_device *pci_first(void);

/* Physical address of BAR `idx` (MMIO address or IO port). 0 if unimplemented. */
uint64_t pci_bar_addr(const struct pci_device *p, int idx);

#endif
