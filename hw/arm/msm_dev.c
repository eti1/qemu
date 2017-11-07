#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "hw/arm/primecell.h"
#include "hw/arm/virt.h"
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
	fprintf(stderr,"msm_dev_sec(%p): read %d at 0x%x\n", opaque,size, (unsigned)offset);
    return 0;
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
