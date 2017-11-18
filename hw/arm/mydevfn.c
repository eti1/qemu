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

uint64_t msm_dev_sec_read(void *opaque, hwaddr offset,
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
