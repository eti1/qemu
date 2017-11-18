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

uint64_t devfn_sec_ctrl_read(void *opaque, hwaddr offset,
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

	fprintf(stderr,"dev_sec: read %d at 0x%x -> 0x%llx\n", size, (unsigned)offset, v);

    return v;
}

static rpm_delay_state = 0;
void devfn_rpm_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
    fprintf(stderr,"dev rpm(00060000): write %d: 0x%x at 0x%x\n", size, (unsigned)value,(unsigned) offset);
	switch(offset)
	{
	case 0x200c:
		rpm_delay_state = 0;
		break;
	default:
		break;
	}
}

uint64_t devfn_rpm_read(void *opaque, hwaddr offset,
                            unsigned size)
{
	uint64_t v;

	switch(offset)
	{
	case 0x2004:
		/* Elapsed timer :::::::*/
		switch(rpm_delay_state)
		{
		case 0:
			rpm_delay_state = 1;
			v = 0;
			break;
		case 1:
			v = 0x40000000;
			break;
		default:
			break;
		}
		break;
	default:
		v = 0;
		break;
	}

	fprintf(stderr,"dev_rpm: read %d at 0x%x -> 0x%llx\n", size, (unsigned)offset, v);

    return v;
}

uint64_t devfn_sdc3_bam_read(VirtMachineState *vms, hwaddr offset,
                            unsigned size)
{
    fprintf(stderr,"dev sdc3_bam(12182000): read %d at 0x%x\n", size, (unsigned)offset);
    return 0;
}

void devfn_sdc3_bam_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
    fprintf(stderr,"dev sdc3_bam(12182000): write %d: 0x%x at 0x%x\n", size, (unsigned)value,(unsigned) offset);
}
