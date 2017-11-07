#if 0
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qemu/log.h"
#include "cpu.h"
#include "hw/cpu/a9mpcore.h"
#include "hw/boards.h"
#include "sysemu/sysemu.h"
#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "hw/loader.h"

#define ROM_ADDR		0
#define ROM_SIZE		0x20000
#define RAM_ADDR		0x20000
#define RAM_SIZE		0x28000
#define MY_DEV_MEM_ADDR 0xFAFA0000
#define MY_DEV_MEM_SIZE 4
#define MY_BOARD_ID		0x123
#define ROM_NAME		"my_rom_file"

static uint64_t my_dev_read(void *opaque, hwaddr offset,
                            unsigned size)
{
    return 0x41424344;
}

static void my_dev_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
    return;
}

static const MemoryRegionOps my_dev_ops = {
    .read = my_dev_read,
    .write = my_dev_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .max_access_size = 4,
    }
};

static struct arm_boot_info binfo = {
    .loader_start = ROM_SIZE,
    .board_id = MY_BOARD_ID,
};

void myboard_init(MachineState *machine)
{
	int rom_size;
	Object *cpuobj;
    MemoryRegion *ram_mem;
    MemoryRegion *rom_mem;
    MemoryRegion *dev_mem;
    MemoryRegion *sysmem = get_system_memory();

	ram_mem = g_new(MemoryRegion, 1);
	rom_mem = g_new(MemoryRegion, 1);
	dev_mem = g_new(MemoryRegion, 1);

	cpuobj = object_new(ARM_CPU_TYPE_NAME("cortex-m3"));

#if 0
	/* By default A9 CPUs have EL3 enabled.  This board does not currently
	 * support EL3 so the CPU EL3 property is disabled before realization.
	 */
	if (object_property_find(cpuobj, "has_el3", NULL)) {
		object_property_set_bool(cpuobj, false, "has_el3", &error_fatal);
	}
#endif

    /*** Memory ***/

    /* Test device */
    memory_region_init_io(dev_mem, NULL, &my_dev_ops,
        NULL, "myboard.mydevmem", MY_DEV_MEM_SIZE);
    memory_region_add_subregion(sysmem, MY_DEV_MEM_ADDR,
                                dev_mem);

    /* Internal RAM */
    memory_region_init_ram(ram_mem, NULL, "myboard.ram",
                           RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, RAM_ADDR,
                                ram_mem);

    /* Internal ROM */
    memory_region_init_ram(rom_mem, NULL, "myboard.rom",
                           ROM_SIZE, &error_fatal);
    memory_region_set_readonly(rom_mem, true);
    memory_region_add_subregion(sysmem, ROM_ADDR,
                                rom_mem);

	/* Loading ROM */
	rom_size = get_image_size(ROM_NAME);
	if (rom_size != ROM_SIZE)
	{
		fprintf(stderr, "%s: ROM image invalid (%x != %x)\n",
						__FUNCTION__, rom_size, ROM_SIZE);
		exit(1);
	}
	if (load_image_targphys(ROM_NAME, ROM_ADDR, ROM_SIZE) < 0)
	{
		fprintf(stderr, "%s: error loading '%s'\n",
						__FUNCTION__, ROM_NAME);
		exit(1);
	}

	object_property_set_link(cpuobj, OBJECT(sysmem), "memory",
							 &error_abort);
	object_property_set_bool(cpuobj, true, "realized", NULL);

    arm_load_kernel(ARM_CPU(first_cpu), &binfo);

    return;
}


static void my_machine_init(MachineClass *mc)
{
    mc->desc = "My Machine";
    mc->init = myboard_init;
 //   mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m3");
	
}

DEFINE_MACHINE("myboard", my_machine_init)
#endif
