#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/misc/unimp.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "target/arm/cpu.h"
#include "hw/arm/machines-qom.h"

#define RH1_FLASH_BASE          0x00000000UL   
#define RH1_FLASH_SIZE          (1  * MiB)
#define RH1_SRAM_BASE           0x00800000UL
#define RH1_SRAM_SIZE           (32 * KiB)
#define RH1_BOOTROM_BASE        0x00810000UL
#define RH1_BOOTROM_SIZE        (32 * KiB)
#define RH1_DRAM_BASE           0x02000000UL
#define RH1_DRAM_SIZE           (16 * MiB)
#define RH1_PERIPH_BASE         0x03000000UL
#define RH1_PERIPH_SIZE         (256 * KiB)
#define RH1_ADV_PERIPH_BASE     0x03800000UL
#define RH1_ADV_PERIPH_SIZE     (64  * KiB)

#define TYPE_RH1_MACHINE  MACHINE_TYPE_NAME("rh1")
OBJECT_DECLARE_SIMPLE_TYPE(RH1MachineState, RH1_MACHINE)

struct RH1MachineState {
    MachineState parent_obj;
    ARMCPU *cpu;

    /* Memory regions */
    MemoryRegion flash;
    MemoryRegion sram;
    MemoryRegion bootrom;
    MemoryRegion dram;

    /* Binaries paths */
    char *flash_path;
    char *sram_path;
    char *bootrom_path;
    char *dram_path;

    /* Properties */
    bool boot_rom_mode;
};

#define RH1_REG_SIZE 0x30000 

typedef struct {
    uint32_t regs[RH1_REG_SIZE];
} RH1PeriphState;

static uint64_t rh1_basic_periph_read(void *opaque, hwaddr offset, unsigned size)
{
    RH1PeriphState *s = opaque;
    uint32_t val;

    if (offset < RH1_REG_SIZE) {
        val = s->regs[offset >> 2];
    } else {
        val = 0;
    }

    switch (offset) {
        case 0x04:
            /* Ensure bit 4 is 0 to satisfy FUN_00027e8a */
            val &= ~(1 << 4);
            break;

        case 0x24:
            /* Ensure bit 3 is 1 to satisfy FUN_00027e8a */
            val |= (1 << 3);
            break;

        case 0x200:
            /* Ensure bit 9 is 1 to satisfy FUN_0004d18c */
            val |= (1 << 9);
            break;

        case 0x20094:
            /* Ensure bit 8 is 0 to satisfy FUN_000001ac */
            val &= ~(1 << 8);
            break;
    }

    printf("rh1 read: offset 0x%" HWADDR_PRIx " -> 0x%08x\n", offset, val);
    return val;
}

static void rh1_basic_periph_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    RH1PeriphState *s = opaque;

    printf("rh1 write: offset 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n", offset, val);

    if (!s) {
        fprintf(stderr, "RH1 Error: Peripheral state is NULL!\n");
        return;
    }

    if (offset < RH1_REG_SIZE) {
        s->regs[offset] = (uint32_t)val;
    } else {
        fprintf(stderr, "RH1 Error: Out of bounds write at offset 0x%" HWADDR_PRIx "\n", offset);
    }
}

static const MemoryRegionOps rh1_basic_periph_ops = {
    .read = rh1_basic_periph_read,
    .write = rh1_basic_periph_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static int rh1_load_binary(const char *filename, hwaddr addr, hwaddr max_size, bool required)
{
    int size;

    if (!filename) {
        if (required) {
            error_report("rh1: %s image is required but not specified", filename);
            exit(1);
        }
        return 0;
    }

    size = load_image_targphys(filename, addr, max_size, NULL);
    if (size < 0) {
        if (required) {
            error_report("rh1: failed to load %s '%s'", filename, filename);
            exit(1);
        }
        warn_report("rh1: could not load %s '%s' (ignored)", filename, filename);
        return 0;
    }

    return size;
}

static void rh1_set_flash_path(Object *obj, const char *value, Error **errp)
{
    RH1MachineState *s = RH1_MACHINE(obj);
    g_free(s->flash_path);
    s->flash_path = g_strdup(value);
}

static void rh1_set_sram_path(Object *obj, const char *value, Error **errp)
{
    RH1MachineState *s = RH1_MACHINE(obj);
    g_free(s->sram_path);
    s->sram_path = g_strdup(value);
}

static void rh1_set_bootrom_path(Object *obj, const char *value, Error **errp)
{
    RH1MachineState *s = RH1_MACHINE(obj);
    g_free(s->bootrom_path);
    s->bootrom_path = g_strdup(value);
}

static void rh1_set_dram_path(Object *obj, const char *value, Error **errp)
{
    RH1MachineState *s = RH1_MACHINE(obj);
    g_free(s->dram_path);
    s->dram_path = g_strdup(value);
}

static void rh1_set_boot_mode(Object *obj, bool value, Error **errp)
{
    RH1MachineState *s = RH1_MACHINE(obj);
    s->boot_rom_mode = value;
}

static void rh1_init(MachineState *machine)
{
    RH1MachineState *s = RH1_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();

    s->cpu = ARM_CPU(cpu_create(machine->cpu_type));

    memory_region_init_ram(&s->flash, NULL, "rh1.flash", RH1_FLASH_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, RH1_FLASH_BASE, &s->flash);

    memory_region_init_ram(&s->sram, NULL, "rh1.sram", RH1_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, RH1_SRAM_BASE, &s->sram);

    memory_region_init_ram(&s->bootrom, NULL, "rh1.bootrom", RH1_BOOTROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, RH1_BOOTROM_BASE, &s->bootrom);

    memory_region_init_ram(&s->dram, NULL, "rh1.dram", RH1_DRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, RH1_DRAM_BASE, &s->dram);

    rh1_load_binary(s->flash_path, RH1_FLASH_BASE, RH1_FLASH_SIZE, true);
    rh1_load_binary(s->sram_path, RH1_SRAM_BASE, RH1_SRAM_SIZE, false);
    rh1_load_binary(s->bootrom_path, RH1_BOOTROM_BASE, RH1_BOOTROM_SIZE, false);
    rh1_load_binary(s->dram_path, RH1_DRAM_BASE, RH1_DRAM_SIZE, false);

    MemoryRegion *basic_periph = g_new(MemoryRegion, 1);
    memory_region_init_io(basic_periph, NULL, &rh1_basic_periph_ops, machine, 
                        "rh1.periph.basic", RH1_PERIPH_SIZE);
    memory_region_add_subregion(sysmem, RH1_PERIPH_BASE, basic_periph);
    create_unimplemented_device("rh1.periph.advanced",
                                RH1_ADV_PERIPH_BASE, RH1_ADV_PERIPH_SIZE);

    memory_region_set_readonly(&s->flash, true);
    memory_region_set_readonly(&s->bootrom, true);

    // HSALF low
    if (s->boot_rom_mode) {
        MemoryRegion *br_alias = g_new0(MemoryRegion, 1);
        memory_region_init_alias(br_alias, NULL, "rh1.bootrom.alias0",
                                 &s->bootrom, 0, RH1_BOOTROM_SIZE);
        memory_region_add_subregion_overlap(sysmem, RH1_FLASH_BASE,
                                            br_alias, 1);
    }
}

static void rh1_machine_class_init(ObjectClass *klass, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    mc->desc = "CXD2687";
    mc->init = rh1_init;

    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm7tdmi");

    mc->default_ram_size = 0;

    mc->ignore_memory_transaction_failures = false;

    object_class_property_add_str(klass, "flash", NULL, rh1_set_flash_path);
    object_class_property_add_str(klass, "sram", NULL, rh1_set_sram_path);
    object_class_property_add_str(klass, "bootrom", NULL, rh1_set_bootrom_path);
    object_class_property_add_str(klass, "dram", NULL, rh1_set_dram_path);
    object_class_property_add_bool(klass, "boot_mode", false, rh1_set_boot_mode);
}

static const TypeInfo rh1_machine_type = {
    .name = TYPE_RH1_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(RH1MachineState),
    .class_init = rh1_machine_class_init,
    .interfaces = arm_machine_interfaces,
};

static void rh1_machine_register_type(void)
{
    type_register_static(&rh1_machine_type);
}

type_init(rh1_machine_register_type)
