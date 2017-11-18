#ifndef MSM_DEV_H
#define MSM_DEV_H
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

enum {
    VIRT_FLASH,
	VIRT_MSM_RPM_RAM,
	VIRT_MSM_RTC,
	VIRT_MSM_SEC,
	VIRT_MSM_RPM_TIMERS,
	VIRT_MSM_TMR0,
	VIRT_MSM_PMIC,
    VIRT_CPUPERIPHS,
    VIRT_GIC_DIST,
    VIRT_GIC_CPU,
    VIRT_GIC_V2M,
    VIRT_GIC_ITS,
    VIRT_GIC_REDIST,
    VIRT_RTC,
    VIRT_PLATFORM_BUS,
};

#define RAMLIMIT_GB 255
#define RAMLIMIT_BYTES (RAMLIMIT_GB * 1024ULL * 1024 * 1024)

void create_msm_peripherals(VirtMachineState *vms, MemoryRegion *sysmem);

#endif
