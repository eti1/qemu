#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "hw/arm/primecell.h"
#include "hw/arm/myvirt.h"
#include "hw/devices.h"
#include "net/net.h"
#include "sysemu/block-backend.h"
#include "sysemu/device_tree.h"
#include "sysemu/numa.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "hw/compat.h"
#include "hw/loader.h"
#include "exec/address-spaces.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "hw/pci-host/gpex.h"
#include "hw/arm/sysbus-fdt.h"
#include "hw/platform-bus.h"
#include "hw/arm/fdt.h"
#include "hw/intc/arm_gic.h"
#include "hw/intc/arm_gicv3_common.h"
#include "kvm_arm.h"
#include "hw/smbios/smbios.h"
#include "qapi/visitor.h"
#include "standard-headers/linux/input.h"

#include "msm_dev.h"

const MemMapEntry msm_memmap[] = {
    /* Space up to 0x8000000 is reserved for a boot ROM */
    [VIRT_FLASH] =              {          0, 0x00020000 },
    [VIRT_CPUPERIPHS] =         { 0x08000000, 0x00020000 },
    /* GIC distributor and CPU interfaces sit inside the CPU peripheral space */
    [VIRT_GIC_DIST] =           { 0x08000000, 0x00010000 },
    [VIRT_GIC_CPU] =            { 0x08010000, 0x00010000 },
    [VIRT_GIC_V2M] =            { 0x08020000, 0x00001000 },
    /* The space in between here is reserved for GICv3 CPU/vCPU/HYP */
    [VIRT_GIC_ITS] =            { 0x08080000, 0x00020000 },
    /* This redistributor space allows up to 2*64kB*123 CPUs */
    [VIRT_GIC_REDIST] =         { 0x080A0000, 0x00F60000 },
    [VIRT_UART] =               { 0x09000000, 0x00001000 },
    [VIRT_RTC] =                { 0x09010000, 0x00001000 },
    [VIRT_FW_CFG] =             { 0x09020000, 0x00000018 },
    [VIRT_GPIO] =               { 0x09030000, 0x00001000 },
    [VIRT_MMIO] =               { 0x0a000000, 0x00000200 },
    /* ...repeating for a total of NUM_VIRTIO_TRANSPORTS, each of that size */
    [VIRT_PLATFORM_BUS] =       { 0x0c000000, 0x02000000 },
    [VIRT_MSM_RPM_RAM] =		{ 0x00020000, 0x00028000 },
    [VIRT_MSM_TMR0] =			{ 0x00062000, 0x00001000 },
    [VIRT_MSM_RPM_TIMERS] =		{ 0x00063000, 0x00001000 },
    [VIRT_MSM_PMIC] =			{ 0x00200000, 0x00001000 },
    [VIRT_MSM_SEC] =			{ 0x00700000, 0x00008000 },
    [VIRT_MSM_RTC] =			{ 0x00902000, 0x00001000 },
};

const int msm_irqmap[] = {
    [VIRT_UART] = 1,
    [VIRT_RTC] = 2,
    [VIRT_GPIO] = 7,
    [VIRT_MMIO] = 16, /* ...to 16 + NUM_VIRTIO_TRANSPORTS - 1 */
    [VIRT_GIC_V2M] = 48, /* ...to 48 + NUM_GICV2M_SPIS - 1 */
    [VIRT_PLATFORM_BUS] = 112, /* ...to 112 + PLATFORM_BUS_NUM_IRQS -1 */
};

static uint64_t msm_dev_rtc_read(void *opaque, hwaddr offset,
                            unsigned size)
{
	fprintf(stderr,"msm_dev_rtc(%p): read %d at 0x%x\n", opaque,size, (unsigned)offset);
    return 0;
}

static void msm_dev_rtc_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
	fprintf(stderr,"msm_dev_rtc(%p): write %d: 0x%x at 0x%x\n", opaque,size, (unsigned)value,(unsigned) offset);
    return;
}

const MemoryRegionOps msm_dev_rtc_ops = {
    .read = msm_dev_rtc_read,
    .write = msm_dev_rtc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .max_access_size = 4,
    }
};

static uint64_t msm_dev_sec_read(void *opaque, hwaddr offset,
                            unsigned size)
{
	uint64_t v;

	switch(offset)
	{
	case 0x6034:
		/* Start from rom */
		v = 1;
		break;
	default:
		v = 0;
		break;
	}

	fprintf(stderr,"msm_dev_sec(%p): read %d at 0x%x -> 0x%llx\n", opaque,size, (unsigned)offset, v);

    return v;
}

static void msm_dev_sec_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
	fprintf(stderr,"msm_dev_sec(%p): write %d: 0x%x at 0x%x\n", opaque,size, (unsigned)value,(unsigned) offset);
    return;
}

const MemoryRegionOps msm_dev_sec_ops = {
    .read = msm_dev_sec_read,
    .write = msm_dev_sec_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .max_access_size = 4,
    }
};

static uint64_t msm_dev_timers_read(void *opaque, hwaddr offset,
                            unsigned size)
{
	fprintf(stderr,"msm_dev_timers(%p): read %d at 0x%x\n", opaque,size, (unsigned)offset);
    return 0;
}

static void msm_dev_timers_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
	fprintf(stderr,"msm_dev_timers(%p): write %d: 0x%x at 0x%x\n", opaque,size, (unsigned)value,(unsigned) offset);
    return;
}

const MemoryRegionOps msm_dev_timers_ops = {
    .read = msm_dev_timers_read,
    .write = msm_dev_timers_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .max_access_size = 4,
    }
};

static uint64_t msm_dev_tmr0_read(void *opaque, hwaddr offset,
                            unsigned size)
{
	fprintf(stderr,"msm_dev_tmr0(%p): read %d at 0x%x\n", opaque,size, (unsigned)offset);
    return 0;
}

static void msm_dev_tmr0_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
	fprintf(stderr,"msm_dev_tmr0(%p): write %d: 0x%x at 0x%x\n", opaque,size, (unsigned)value,(unsigned) offset);
    return;
}

const MemoryRegionOps msm_dev_tmr0_ops = {
    .read = msm_dev_tmr0_read,
    .write = msm_dev_tmr0_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .max_access_size = 4,
    }
};

static uint64_t msm_dev_pmic_read(void *opaque, hwaddr offset,
                            unsigned size)
{
	fprintf(stderr,"msm_dev_pmic(%p): read %d at 0x%x\n", opaque,size, (unsigned)offset);
    return 0;
}

static void msm_dev_pmic_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
	fprintf(stderr,"msm_dev_pmic(%p): write %d: 0x%x at 0x%x\n", opaque,size, (unsigned)value,(unsigned) offset);
    return;
}

const MemoryRegionOps msm_dev_pmic_ops = {
    .read = msm_dev_pmic_read,
    .write = msm_dev_pmic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .max_access_size = 4,
    }
};

static void create_msm_memdev(const VirtMachineState *vms,
							MemoryRegion *mem,
							int region,
							const MemoryRegionOps *dev_ops,
							const char *devname,
							void *opaque)
{
    MemoryRegion *dev_mem;
	hwaddr base, size;

	base = vms->memmap[region].base;
	size = vms->memmap[region].size;

	fprintf(stderr,"create my dev: %08x - %08x\n", (unsigned) base, (unsigned) size);

	dev_mem = g_new(MemoryRegion, 1);

    memory_region_init_io(dev_mem, NULL, dev_ops,
        opaque, devname, size);
    memory_region_add_subregion(mem, base, dev_mem);
}

void create_msm_peripherals(VirtMachineState *vms, MemoryRegion *sysmem)
{
	create_msm_memdev(vms, sysmem, VIRT_MSM_RTC, &msm_dev_rtc_ops, "msm_rtc.mem", (void*)0xDE35347C);
	create_msm_memdev(vms, sysmem, VIRT_MSM_SEC, &msm_dev_sec_ops, "msm_sec.mem", (void*)0xDE3535EC);
	create_msm_memdev(vms, sysmem, VIRT_MSM_RPM_TIMERS, &msm_dev_timers_ops, "msm_timers.mem", (void*)0xDE353734);
	create_msm_memdev(vms, sysmem, VIRT_MSM_TMR0, &msm_dev_tmr0_ops, "msm_tmr0.mem", (void*)0xDE353730);
	create_msm_memdev(vms, sysmem, VIRT_MSM_PMIC, &msm_dev_pmic_ops, "msm_pmic.mem", (void*)0xDE35931C);
}
