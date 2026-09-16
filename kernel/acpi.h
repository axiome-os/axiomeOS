#ifndef AXIOME_ACPI_H
#define AXIOME_ACPI_H

#include <stdint.h>

struct acpi_rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem[6];
    char     oem_table[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_addr;
    uint32_t flags;
    uint8_t  entries[];
} __attribute__((packed));

/* 12-byte ACPI Generic Address Structure (registers in the FACP). */
struct acpi_generic_address {
    uint8_t  space_id;   /* 0 = SystemMemory, 1 = SystemIO */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

/* FACP fields used for power control. Packed so the pm1_* and reset_reg
   members land on the offsets ACPI mandates (pm1a_cnt_blk = 64,
   pm1b_cnt_blk = 68, reset_reg = 116). */
struct acpi_fadt {
    struct acpi_sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved1;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved2;
    uint32_t flags;
    struct acpi_generic_address reset_reg;
    uint8_t  reset_value;
} __attribute__((packed));

#define MADT_TYPE_LOCAL_APIC      0
#define MADT_TYPE_IO_APIC         1
#define MADT_TYPE_ISO             2
#define MADT_TYPE_NMI             4

struct madt_io_apic {
    uint8_t  type;
    uint8_t  length;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_iso {
    uint8_t  type;
    uint8_t  length;
    uint8_t  bus_src;
    uint8_t  irq_src;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

void acpi_init(void *rsdp_addr);
int  acpi_ioapic_count(void);
int  acpi_ioapic_info(int idx, uint64_t *addr, uint32_t *gsi_base);
int  acpi_iso_lookup(uint8_t irq, uint32_t *gsi, uint16_t *flags);

/* System power control (called from sys_reboot / sys_poweroff). Best-effort
   on bare metal; reliable on QEMU. acpi_poweroff() falls back to a reset if
   ACPI S5 hardware is unavailable. */
void acpi_reboot(void);
void acpi_poweroff(void);

#endif