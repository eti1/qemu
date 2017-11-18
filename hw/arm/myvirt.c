/*
 * ARM mach-virt emulation
 *
 * Copyright (c) 2013 Linaro Limited
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Emulate a virtual board which works by passing Linux all the information
 * it needs about what devices are present via the device tree.
 * There are some restrictions about what we can do here:
 *  + we can only present devices whose Linux drivers will work based
 *    purely on the device tree with no platform data at all
 *  + we want to present a very stripped-down minimalist platform,
 *    both because this reduces the security attack surface from the guest
 *    and also because it reduces our exposure to being broken when
 *    the kernel updates its device tree bindings and requires further
 *    information in a device binding that we aren't providing.
 * This is essentially the same approach kvmtool uses.
 */

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

#define DEFINE_VIRT_MACHINE_LATEST(major, minor, latest) \
    static void myvirt_##major##_##minor##_class_init(ObjectClass *oc, \
                                                    void *data) \
    { \
        MachineClass *mc = MACHINE_CLASS(oc); \
        myvirt_machine_##major##_##minor##_options(mc); \
        mc->desc = "QEMU " # major "." # minor " ARM Virtual Machine"; \
        if (latest) { \
            mc->alias = "myvirt"; \
        } \
    } \
    static const TypeInfo machmyvirt_##major##_##minor##_info = { \
        .name = MACHINE_TYPE_NAME("myvirt-" # major "." # minor), \
        .parent = TYPE_VIRT_MACHINE, \
        .instance_init = myvirt_##major##_##minor##_instance_init, \
        .class_init = myvirt_##major##_##minor##_class_init, \
    }; \
    static void machmyvirt_machine_##major##_##minor##_init(void) \
    { \
        type_register_static(&machmyvirt_##major##_##minor##_info); \
    } \
    type_init(machmyvirt_machine_##major##_##minor##_init);

#define DEFINE_VIRT_MACHINE_AS_LATEST(major, minor) \
    DEFINE_VIRT_MACHINE_LATEST(major, minor, true)


/* Number of external interrupt lines to configure the GIC with */
#define NUM_IRQS 256

#define PLATFORM_BUS_NUM_IRQS 64

static ARMPlatformBusSystemParams platform_bus_params;

extern const MemMapEntry msm_memmap[];
extern const int msm_irqmap[];

static const char *valid_cpus[] = {
    ARM_CPU_TYPE_NAME("cortex-a15"),
    ARM_CPU_TYPE_NAME("cortex-a53"),
    ARM_CPU_TYPE_NAME("cortex-a57"),
    ARM_CPU_TYPE_NAME("host"),
};

static bool cpu_type_valid(const char *cpu)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(valid_cpus); i++) {
        if (strcmp(cpu, valid_cpus[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void create_fdt(VirtMachineState *vms)
{
    void *fdt = create_device_tree(&vms->fdt_size);

    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }

    vms->fdt = fdt;

    /* Header */
    qemu_fdt_setprop_string(fdt, "/", "compatible", "linux,dummy-virt");
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);

    /*
     * /chosen and /memory nodes must exist for load_dtb
     * to fill in necessary properties later
     */
    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_add_subnode(fdt, "/memory");
    qemu_fdt_setprop_string(fdt, "/memory", "device_type", "memory");

    if (have_numa_distance) {
        int size = nb_numa_nodes * nb_numa_nodes * 3 * sizeof(uint32_t);
        uint32_t *matrix = g_malloc0(size);
        int idx, i, j;

        for (i = 0; i < nb_numa_nodes; i++) {
            for (j = 0; j < nb_numa_nodes; j++) {
                idx = (i * nb_numa_nodes + j) * 3;
                matrix[idx + 0] = cpu_to_be32(i);
                matrix[idx + 1] = cpu_to_be32(j);
                matrix[idx + 2] = cpu_to_be32(numa_info[i].distance[j]);
            }
        }

        qemu_fdt_add_subnode(fdt, "/distance-map");
        qemu_fdt_setprop_string(fdt, "/distance-map", "compatible",
                                "numa-distance-map-v1");
        qemu_fdt_setprop(fdt, "/distance-map", "distance-matrix",
                         matrix, size);
        g_free(matrix);
    }
}

static void fdt_add_psci_node(const VirtMachineState *vms)
{
    uint32_t cpu_suspend_fn;
    uint32_t cpu_off_fn;
    uint32_t cpu_on_fn;
    uint32_t migrate_fn;
    void *fdt = vms->fdt;
    ARMCPU *armcpu = ARM_CPU(qemu_get_cpu(0));
    const char *psci_method;

    switch (vms->psci_conduit) {
    case QEMU_PSCI_CONDUIT_DISABLED:
        return;
    case QEMU_PSCI_CONDUIT_HVC:
        psci_method = "hvc";
        break;
    case QEMU_PSCI_CONDUIT_SMC:
        psci_method = "smc";
        break;
    default:
        g_assert_not_reached();
    }

    qemu_fdt_add_subnode(fdt, "/psci");
    if (armcpu->psci_version == 2) {
        const char comp[] = "arm,psci-0.2\0arm,psci";
        qemu_fdt_setprop(fdt, "/psci", "compatible", comp, sizeof(comp));

        cpu_off_fn = QEMU_PSCI_0_2_FN_CPU_OFF;
        if (arm_feature(&armcpu->env, ARM_FEATURE_AARCH64)) {
            cpu_suspend_fn = QEMU_PSCI_0_2_FN64_CPU_SUSPEND;
            cpu_on_fn = QEMU_PSCI_0_2_FN64_CPU_ON;
            migrate_fn = QEMU_PSCI_0_2_FN64_MIGRATE;
        } else {
            cpu_suspend_fn = QEMU_PSCI_0_2_FN_CPU_SUSPEND;
            cpu_on_fn = QEMU_PSCI_0_2_FN_CPU_ON;
            migrate_fn = QEMU_PSCI_0_2_FN_MIGRATE;
        }
    } else {
        qemu_fdt_setprop_string(fdt, "/psci", "compatible", "arm,psci");

        cpu_suspend_fn = QEMU_PSCI_0_1_FN_CPU_SUSPEND;
        cpu_off_fn = QEMU_PSCI_0_1_FN_CPU_OFF;
        cpu_on_fn = QEMU_PSCI_0_1_FN_CPU_ON;
        migrate_fn = QEMU_PSCI_0_1_FN_MIGRATE;
    }

    /* We adopt the PSCI spec's nomenclature, and use 'conduit' to refer
     * to the instruction that should be used to invoke PSCI functions.
     * However, the device tree binding uses 'method' instead, so that is
     * what we should use here.
     */
    qemu_fdt_setprop_string(fdt, "/psci", "method", psci_method);

    qemu_fdt_setprop_cell(fdt, "/psci", "cpu_suspend", cpu_suspend_fn);
    qemu_fdt_setprop_cell(fdt, "/psci", "cpu_off", cpu_off_fn);
    qemu_fdt_setprop_cell(fdt, "/psci", "cpu_on", cpu_on_fn);
    qemu_fdt_setprop_cell(fdt, "/psci", "migrate", migrate_fn);
}

static void fdt_add_timer_nodes(const VirtMachineState *vms)
{
    /* On real hardware these interrupts are level-triggered.
     * On KVM they were edge-triggered before host kernel version 4.4,
     * and level-triggered afterwards.
     * On emulated QEMU they are level-triggered.
     *
     * Getting the DTB info about them wrong is awkward for some
     * guest kernels:
     *  pre-4.8 ignore the DT and leave the interrupt configured
     *   with whatever the GIC reset value (or the bootloader) left it at
     *  4.8 before rc6 honour the incorrect data by programming it back
     *   into the GIC, causing problems
     *  4.8rc6 and later ignore the DT and always write "level triggered"
     *   into the GIC
     *
     * For backwards-compatibility, virt-2.8 and earlier will continue
     * to say these are edge-triggered, but later machines will report
     * the correct information.
     */
    ARMCPU *armcpu;
    VirtMachineClass *vmc = VIRT_MACHINE_GET_CLASS(vms);
    uint32_t irqflags = GIC_FDT_IRQ_FLAGS_LEVEL_HI;

    if (vmc->claim_edge_triggered_timers) {
        irqflags = GIC_FDT_IRQ_FLAGS_EDGE_LO_HI;
    }

    if (vms->gic_version == 2) {
        irqflags = deposit32(irqflags, GIC_FDT_IRQ_PPI_CPU_START,
                             GIC_FDT_IRQ_PPI_CPU_WIDTH,
                             (1 << vms->smp_cpus) - 1);
    }

    qemu_fdt_add_subnode(vms->fdt, "/timer");

    armcpu = ARM_CPU(qemu_get_cpu(0));
    if (arm_feature(&armcpu->env, ARM_FEATURE_V8)) {
        const char compat[] = "arm,armv8-timer\0arm,armv7-timer";
        qemu_fdt_setprop(vms->fdt, "/timer", "compatible",
                         compat, sizeof(compat));
    } else {
        qemu_fdt_setprop_string(vms->fdt, "/timer", "compatible",
                                "arm,armv7-timer");
    }
    qemu_fdt_setprop(vms->fdt, "/timer", "always-on", NULL, 0);
    qemu_fdt_setprop_cells(vms->fdt, "/timer", "interrupts",
                       GIC_FDT_IRQ_TYPE_PPI, ARCH_TIMER_S_EL1_IRQ, irqflags,
                       GIC_FDT_IRQ_TYPE_PPI, ARCH_TIMER_NS_EL1_IRQ, irqflags,
                       GIC_FDT_IRQ_TYPE_PPI, ARCH_TIMER_VIRT_IRQ, irqflags,
                       GIC_FDT_IRQ_TYPE_PPI, ARCH_TIMER_NS_EL2_IRQ, irqflags);
}

static void fdt_add_cpu_nodes(const VirtMachineState *vms)
{
    int cpu;
    int addr_cells = 1;
    const MachineState *ms = MACHINE(vms);

    /*
     * From Documentation/devicetree/bindings/arm/cpus.txt
     *  On ARM v8 64-bit systems value should be set to 2,
     *  that corresponds to the MPIDR_EL1 register size.
     *  If MPIDR_EL1[63:32] value is equal to 0 on all CPUs
     *  in the system, #address-cells can be set to 1, since
     *  MPIDR_EL1[63:32] bits are not used for CPUs
     *  identification.
     *
     *  Here we actually don't know whether our system is 32- or 64-bit one.
     *  The simplest way to go is to examine affinity IDs of all our CPUs. If
     *  at least one of them has Aff3 populated, we set #address-cells to 2.
     */
    for (cpu = 0; cpu < vms->smp_cpus; cpu++) {
        ARMCPU *armcpu = ARM_CPU(qemu_get_cpu(cpu));

        if (armcpu->mp_affinity & ARM_AFF3_MASK) {
            addr_cells = 2;
            break;
        }
    }

    qemu_fdt_add_subnode(vms->fdt, "/cpus");
    qemu_fdt_setprop_cell(vms->fdt, "/cpus", "#address-cells", addr_cells);
    qemu_fdt_setprop_cell(vms->fdt, "/cpus", "#size-cells", 0x0);

    for (cpu = vms->smp_cpus - 1; cpu >= 0; cpu--) {
        char *nodename = g_strdup_printf("/cpus/cpu@%d", cpu);
        ARMCPU *armcpu = ARM_CPU(qemu_get_cpu(cpu));
        CPUState *cs = CPU(armcpu);

        qemu_fdt_add_subnode(vms->fdt, nodename);
        qemu_fdt_setprop_string(vms->fdt, nodename, "device_type", "cpu");
        qemu_fdt_setprop_string(vms->fdt, nodename, "compatible",
                                    armcpu->dtb_compatible);

        if (vms->psci_conduit != QEMU_PSCI_CONDUIT_DISABLED
            && vms->smp_cpus > 1) {
            qemu_fdt_setprop_string(vms->fdt, nodename,
                                        "enable-method", "psci");
        }

        if (addr_cells == 2) {
            qemu_fdt_setprop_u64(vms->fdt, nodename, "reg",
                                 armcpu->mp_affinity);
        } else {
            qemu_fdt_setprop_cell(vms->fdt, nodename, "reg",
                                  armcpu->mp_affinity);
        }

        if (ms->possible_cpus->cpus[cs->cpu_index].props.has_node_id) {
            qemu_fdt_setprop_cell(vms->fdt, nodename, "numa-node-id",
                ms->possible_cpus->cpus[cs->cpu_index].props.node_id);
        }

        g_free(nodename);
    }
}

static void fdt_add_its_gic_node(VirtMachineState *vms)
{
    vms->msi_phandle = qemu_fdt_alloc_phandle(vms->fdt);
    qemu_fdt_add_subnode(vms->fdt, "/intc/its");
    qemu_fdt_setprop_string(vms->fdt, "/intc/its", "compatible",
                            "arm,gic-v3-its");
    qemu_fdt_setprop(vms->fdt, "/intc/its", "msi-controller", NULL, 0);
    qemu_fdt_setprop_sized_cells(vms->fdt, "/intc/its", "reg",
                                 2, vms->memmap[VIRT_GIC_ITS].base,
                                 2, vms->memmap[VIRT_GIC_ITS].size);
    qemu_fdt_setprop_cell(vms->fdt, "/intc/its", "phandle", vms->msi_phandle);
}

static void fdt_add_v2m_gic_node(VirtMachineState *vms)
{
    vms->msi_phandle = qemu_fdt_alloc_phandle(vms->fdt);
    qemu_fdt_add_subnode(vms->fdt, "/intc/v2m");
    qemu_fdt_setprop_string(vms->fdt, "/intc/v2m", "compatible",
                            "arm,gic-v2m-frame");
    qemu_fdt_setprop(vms->fdt, "/intc/v2m", "msi-controller", NULL, 0);
    qemu_fdt_setprop_sized_cells(vms->fdt, "/intc/v2m", "reg",
                                 2, vms->memmap[VIRT_GIC_V2M].base,
                                 2, vms->memmap[VIRT_GIC_V2M].size);
    qemu_fdt_setprop_cell(vms->fdt, "/intc/v2m", "phandle", vms->msi_phandle);
}

static void fdt_add_gic_node(VirtMachineState *vms)
{
    vms->gic_phandle = qemu_fdt_alloc_phandle(vms->fdt);
    qemu_fdt_setprop_cell(vms->fdt, "/", "interrupt-parent", vms->gic_phandle);

    qemu_fdt_add_subnode(vms->fdt, "/intc");
    qemu_fdt_setprop_cell(vms->fdt, "/intc", "#interrupt-cells", 3);
    qemu_fdt_setprop(vms->fdt, "/intc", "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(vms->fdt, "/intc", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(vms->fdt, "/intc", "#size-cells", 0x2);
    qemu_fdt_setprop(vms->fdt, "/intc", "ranges", NULL, 0);
    if (vms->gic_version == 3) {
        qemu_fdt_setprop_string(vms->fdt, "/intc", "compatible",
                                "arm,gic-v3");
        qemu_fdt_setprop_sized_cells(vms->fdt, "/intc", "reg",
                                     2, vms->memmap[VIRT_GIC_DIST].base,
                                     2, vms->memmap[VIRT_GIC_DIST].size,
                                     2, vms->memmap[VIRT_GIC_REDIST].base,
                                     2, vms->memmap[VIRT_GIC_REDIST].size);
        if (vms->virt) {
            qemu_fdt_setprop_cells(vms->fdt, "/intc", "interrupts",
                                   GIC_FDT_IRQ_TYPE_PPI, ARCH_GICV3_MAINT_IRQ,
                                   GIC_FDT_IRQ_FLAGS_LEVEL_HI);
        }
    } else {
        /* 'cortex-a15-gic' means 'GIC v2' */
        qemu_fdt_setprop_string(vms->fdt, "/intc", "compatible",
                                "arm,cortex-a15-gic");
        qemu_fdt_setprop_sized_cells(vms->fdt, "/intc", "reg",
                                      2, vms->memmap[VIRT_GIC_DIST].base,
                                      2, vms->memmap[VIRT_GIC_DIST].size,
                                      2, vms->memmap[VIRT_GIC_CPU].base,
                                      2, vms->memmap[VIRT_GIC_CPU].size);
    }

    qemu_fdt_setprop_cell(vms->fdt, "/intc", "phandle", vms->gic_phandle);
}

static void fdt_add_pmu_nodes(const VirtMachineState *vms)
{
    CPUState *cpu;
    ARMCPU *armcpu;
    uint32_t irqflags = GIC_FDT_IRQ_FLAGS_LEVEL_HI;

    CPU_FOREACH(cpu) {
        armcpu = ARM_CPU(cpu);
        if (!arm_feature(&armcpu->env, ARM_FEATURE_PMU)) {
            return;
        }
        if (kvm_enabled()) {
            if (kvm_irqchip_in_kernel()) {
                kvm_arm_pmu_set_irq(cpu, PPI(VIRTUAL_PMU_IRQ));
            }
            kvm_arm_pmu_init(cpu);
        }
    }

    if (vms->gic_version == 2) {
        irqflags = deposit32(irqflags, GIC_FDT_IRQ_PPI_CPU_START,
                             GIC_FDT_IRQ_PPI_CPU_WIDTH,
                             (1 << vms->smp_cpus) - 1);
    }

    armcpu = ARM_CPU(qemu_get_cpu(0));
    qemu_fdt_add_subnode(vms->fdt, "/pmu");
    if (arm_feature(&armcpu->env, ARM_FEATURE_V8)) {
        const char compat[] = "arm,armv8-pmuv3";
        qemu_fdt_setprop(vms->fdt, "/pmu", "compatible",
                         compat, sizeof(compat));
        qemu_fdt_setprop_cells(vms->fdt, "/pmu", "interrupts",
                               GIC_FDT_IRQ_TYPE_PPI, VIRTUAL_PMU_IRQ, irqflags);
    }
}

static void create_its(VirtMachineState *vms, DeviceState *gicdev)
{
    const char *itsclass = its_class_name();
    DeviceState *dev;

    if (!itsclass) {
        /* Do nothing if not supported */
        return;
    }

    dev = qdev_create(NULL, itsclass);

    object_property_set_link(OBJECT(dev), OBJECT(gicdev), "parent-gicv3",
                             &error_abort);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, vms->memmap[VIRT_GIC_ITS].base);

    fdt_add_its_gic_node(vms);
}

static void create_v2m(VirtMachineState *vms, qemu_irq *pic)
{
    int i;
    int irq = vms->irqmap[VIRT_GIC_V2M];
    DeviceState *dev;

    dev = qdev_create(NULL, "arm-gicv2m");
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, vms->memmap[VIRT_GIC_V2M].base);
    qdev_prop_set_uint32(dev, "base-spi", irq);
    qdev_prop_set_uint32(dev, "num-spi", NUM_GICV2M_SPIS);
    qdev_init_nofail(dev);

    for (i = 0; i < NUM_GICV2M_SPIS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, pic[irq + i]);
    }

    fdt_add_v2m_gic_node(vms);
}

static void create_gic(VirtMachineState *vms, qemu_irq *pic)
{
    /* We create a standalone GIC */
    DeviceState *gicdev;
    SysBusDevice *gicbusdev;
    const char *gictype;
    int type = vms->gic_version, i;

    gictype = (type == 3) ? gicv3_class_name() : gic_class_name();

    gicdev = qdev_create(NULL, gictype);
    qdev_prop_set_uint32(gicdev, "revision", type);
    qdev_prop_set_uint32(gicdev, "num-cpu", smp_cpus);
    /* Note that the num-irq property counts both internal and external
     * interrupts; there are always 32 of the former (mandated by GIC spec).
     */
    qdev_prop_set_uint32(gicdev, "num-irq", NUM_IRQS + 32);
    if (!kvm_irqchip_in_kernel()) {
        qdev_prop_set_bit(gicdev, "has-security-extensions", vms->secure);
    }
    qdev_init_nofail(gicdev);
    gicbusdev = SYS_BUS_DEVICE(gicdev);
    sysbus_mmio_map(gicbusdev, 0, vms->memmap[VIRT_GIC_DIST].base);
    if (type == 3) {
        sysbus_mmio_map(gicbusdev, 1, vms->memmap[VIRT_GIC_REDIST].base);
    } else {
        sysbus_mmio_map(gicbusdev, 1, vms->memmap[VIRT_GIC_CPU].base);
    }

    /* Wire the outputs from each CPU's generic timer and the GICv3
     * maintenance interrupt signal to the appropriate GIC PPI inputs,
     * and the GIC's IRQ/FIQ/VIRQ/VFIQ interrupt outputs to the CPU's inputs.
     */
    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int ppibase = NUM_IRQS + i * GIC_INTERNAL + GIC_NR_SGIS;
        int irq;
        /* Mapping from the output timer irq lines from the CPU to the
         * GIC PPI inputs we use for the virt board.
         */
        const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
        };

        for (irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(gicdev,
                                                   ppibase + timer_irq[irq]));
        }

        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(gicdev, ppibase
                                                     + ARCH_GICV3_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(gicdev, ppibase
                                                     + VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }

    for (i = 0; i < NUM_IRQS; i++) {
        pic[i] = qdev_get_gpio_in(gicdev, i);
    }

    fdt_add_gic_node(vms);

    if (type == 3 && vms->its) {
        create_its(vms, gicdev);
    } else if (type == 2) {
        create_v2m(vms, pic);
    }
}

static void create_uart(const VirtMachineState *vms, qemu_irq *pic, int uart,
                        MemoryRegion *mem, Chardev *chr)
{
    char *nodename;
    hwaddr base = vms->memmap[uart].base;
    hwaddr size = vms->memmap[uart].size;
    int irq = vms->irqmap[uart];
    const char compat[] = "arm,pl011\0arm,primecell";
    const char clocknames[] = "uartclk\0apb_pclk";
    DeviceState *dev = qdev_create(NULL, "pl011");
    SysBusDevice *s = SYS_BUS_DEVICE(dev);

    qdev_prop_set_chr(dev, "chardev", chr);
    qdev_init_nofail(dev);
    memory_region_add_subregion(mem, base,
                                sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, pic[irq]);

    nodename = g_strdup_printf("/pl011@%" PRIx64, base);
    qemu_fdt_add_subnode(vms->fdt, nodename);
    /* Note that we can't use setprop_string because of the embedded NUL */
    qemu_fdt_setprop(vms->fdt, nodename, "compatible",
                         compat, sizeof(compat));
    qemu_fdt_setprop_sized_cells(vms->fdt, nodename, "reg",
                                     2, base, 2, size);
    qemu_fdt_setprop_cells(vms->fdt, nodename, "interrupts",
                               GIC_FDT_IRQ_TYPE_SPI, irq,
                               GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cells(vms->fdt, nodename, "clocks",
                               vms->clock_phandle, vms->clock_phandle);
    qemu_fdt_setprop(vms->fdt, nodename, "clock-names",
                         clocknames, sizeof(clocknames));

	qemu_fdt_setprop_string(vms->fdt, "/chosen", "stdout-path", nodename);

    g_free(nodename);
}

static void create_rtc(const VirtMachineState *vms, qemu_irq *pic)
{
    char *nodename;
    hwaddr base = vms->memmap[VIRT_RTC].base;
    hwaddr size = vms->memmap[VIRT_RTC].size;
    int irq = vms->irqmap[VIRT_RTC];
    const char compat[] = "arm,pl031\0arm,primecell";

    sysbus_create_simple("pl031", base, pic[irq]);

    nodename = g_strdup_printf("/pl031@%" PRIx64, base);
    qemu_fdt_add_subnode(vms->fdt, nodename);
    qemu_fdt_setprop(vms->fdt, nodename, "compatible", compat, sizeof(compat));
    qemu_fdt_setprop_sized_cells(vms->fdt, nodename, "reg",
                                 2, base, 2, size);
    qemu_fdt_setprop_cells(vms->fdt, nodename, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, irq,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(vms->fdt, nodename, "clocks", vms->clock_phandle);
    qemu_fdt_setprop_string(vms->fdt, nodename, "clock-names", "apb_pclk");
    g_free(nodename);
}

static DeviceState *gpio_key_dev;
static void myvirt_powerdown_req(Notifier *n, void *opaque)
{
    /* use gpio Pin 3 for power button event */
    qemu_set_irq(qdev_get_gpio_in(gpio_key_dev, 0), 1);
}

static Notifier myvirt_system_powerdown_notifier = {
    .notify = myvirt_powerdown_req
};

static void create_one_flash(const char *name, hwaddr flashbase,
                             hwaddr flashsize, const char *file,
                             MemoryRegion *sysmem)
{
    /* Create and map a single flash device. We use the same
     * parameters as the flash devices on the Versatile Express board.
     */
    DriveInfo *dinfo = drive_get_next(IF_PFLASH);
    DeviceState *dev = qdev_create(NULL, "cfi.pflash01");
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    const uint64_t sectorlength = 128 * 1024;

    if (dinfo) {
        qdev_prop_set_drive(dev, "drive", blk_by_legacy_dinfo(dinfo),
                            &error_abort);
    }

	fprintf(stderr,"flashsize: %llx, sectorlength: %llx\n", flashsize, sectorlength);

    qdev_prop_set_uint32(dev, "num-blocks", flashsize / sectorlength);
    qdev_prop_set_uint64(dev, "sector-length", sectorlength);
    qdev_prop_set_uint8(dev, "width", 4);
    qdev_prop_set_uint8(dev, "device-width", 2);
    qdev_prop_set_bit(dev, "big-endian", false);
    qdev_prop_set_uint16(dev, "id0", 0x89);
    qdev_prop_set_uint16(dev, "id1", 0x18);
    qdev_prop_set_uint16(dev, "id2", 0x00);
    qdev_prop_set_uint16(dev, "id3", 0x00);
    qdev_prop_set_string(dev, "name", name);
    qdev_init_nofail(dev);

    memory_region_add_subregion(sysmem, flashbase,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));

    if (file) {
        char *fn;
        int image_size;

        if (drive_get(IF_PFLASH, 0, 0)) {
            error_report("The contents of the first flash device may be "
                         "specified with -bios or with -drive if=pflash... "
                         "but you cannot use both options at once");
            exit(1);
        }
        fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, file);
        if (!fn) {
            error_report("Could not find ROM image '%s'", file);
            exit(1);
        }
        image_size = load_image_mr(fn, sysbus_mmio_get_region(sbd, 0));
        g_free(fn);
        if (image_size < 0) {
            error_report("Could not load ROM image '%s'", file);
            exit(1);
        }
    }
}

static void create_flash(const VirtMachineState *vms,
                         MemoryRegion *sysmem)
{
    hwaddr flashsize = vms->memmap[VIRT_FLASH].size;
    hwaddr flashbase = vms->memmap[VIRT_FLASH].base;
    char *nodename;

    create_one_flash("virt.flash0", flashbase, flashsize,
                     bios_name, sysmem);

	/* Report both flash devices as a single node in the DT */
	nodename = g_strdup_printf("/flash@%" PRIx64, flashbase);
	qemu_fdt_add_subnode(vms->fdt, nodename);
	qemu_fdt_setprop_string(vms->fdt, nodename, "compatible", "cfi-flash");
	qemu_fdt_setprop_sized_cells(vms->fdt, nodename, "reg",
								 2, flashbase, 2, flashsize,
								 2, flashbase + flashsize, 2, flashsize);
	qemu_fdt_setprop_cell(vms->fdt, nodename, "bank-width", 4);
	g_free(nodename);
}

static void create_platform_bus(VirtMachineState *vms, qemu_irq *pic)
{
    DeviceState *dev;
    SysBusDevice *s;
    int i;
    ARMPlatformBusFDTParams *fdt_params = g_new(ARMPlatformBusFDTParams, 1);
    MemoryRegion *sysmem = get_system_memory();

    platform_bus_params.platform_bus_base = vms->memmap[VIRT_PLATFORM_BUS].base;
    platform_bus_params.platform_bus_size = vms->memmap[VIRT_PLATFORM_BUS].size;
    platform_bus_params.platform_bus_first_irq = vms->irqmap[VIRT_PLATFORM_BUS];
    platform_bus_params.platform_bus_num_irqs = PLATFORM_BUS_NUM_IRQS;

    fdt_params->system_params = &platform_bus_params;
    fdt_params->binfo = &vms->bootinfo;
    fdt_params->intc = "/intc";
    /*
     * register a machine init done notifier that creates the device tree
     * nodes of the platform bus and its children dynamic sysbus devices
     */
    arm_register_platform_bus_fdt_creator(fdt_params);

    dev = qdev_create(NULL, TYPE_PLATFORM_BUS_DEVICE);
    dev->id = TYPE_PLATFORM_BUS_DEVICE;
    qdev_prop_set_uint32(dev, "num_irqs",
        platform_bus_params.platform_bus_num_irqs);
    qdev_prop_set_uint32(dev, "mmio_size",
        platform_bus_params.platform_bus_size);
    qdev_init_nofail(dev);
    s = SYS_BUS_DEVICE(dev);

    for (i = 0; i < platform_bus_params.platform_bus_num_irqs; i++) {
        int irqn = platform_bus_params.platform_bus_first_irq + i;
        sysbus_connect_irq(s, i, pic[irqn]);
    }

    memory_region_add_subregion(sysmem,
                                platform_bus_params.platform_bus_base,
                                sysbus_mmio_get_region(s, 0));
}

static void *machmyvirt_dtb(const struct arm_boot_info *binfo, int *fdt_size)
{
    const VirtMachineState *board = container_of(binfo, VirtMachineState,
                                                 bootinfo);

    *fdt_size = board->fdt_size;
    return board->fdt;
}

static void myvirt_build_smbios(VirtMachineState *vms)
{
    uint8_t *smbios_tables, *smbios_anchor;
    size_t smbios_tables_len, smbios_anchor_len;
    const char *product = "QEMU Virtual Machine";

    if (!vms->fw_cfg) {
        return;
    }

    if (kvm_enabled()) {
        product = "KVM Virtual Machine";
    }

    smbios_set_defaults("QEMU", product,
                        "1.0", false, true, SMBIOS_ENTRY_POINT_30);

    smbios_get_tables(NULL, 0, &smbios_tables, &smbios_tables_len,
                      &smbios_anchor, &smbios_anchor_len);

    if (smbios_anchor) {
        fw_cfg_add_file(vms->fw_cfg, "etc/smbios/smbios-tables",
                        smbios_tables, smbios_tables_len);
        fw_cfg_add_file(vms->fw_cfg, "etc/smbios/smbios-anchor",
                        smbios_anchor, smbios_anchor_len);
    }
}

static
void myvirt_machine_done(Notifier *notifier, void *data)
{
    VirtMachineState *vms = container_of(notifier, VirtMachineState,
                                         machine_done);

    virt_acpi_setup(vms);
    myvirt_build_smbios(vms);
}

static uint64_t myvirt_cpu_mp_affinity(VirtMachineState *vms, int idx)
{
    uint8_t clustersz = ARM_DEFAULT_CPUS_PER_CLUSTER;
    VirtMachineClass *vmc = VIRT_MACHINE_GET_CLASS(vms);

    if (!vmc->disallow_affinity_adjustment) {
        /* Adjust MPIDR like 64-bit KVM hosts, which incorporate the
         * GIC's target-list limitations. 32-bit KVM hosts currently
         * always create clusters of 4 CPUs, but that is expected to
         * change when they gain support for gicv3. When KVM is enabled
         * it will override the changes we make here, therefore our
         * purposes are to make TCG consistent (with 64-bit KVM hosts)
         * and to improve SGI efficiency.
         */
        if (vms->gic_version == 3) {
            clustersz = GICV3_TARGETLIST_BITS;
        } else {
            clustersz = GIC_TARGETLIST_BITS;
        }
    }
    return arm_cpu_mp_affinity(idx, clustersz);
}

static void machmyvirt_init(MachineState *machine)
{
    VirtMachineState *vms = VIRT_MACHINE(machine);
    VirtMachineClass *vmc = VIRT_MACHINE_GET_CLASS(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    const CPUArchIdList *possible_cpus;
    qemu_irq pic[NUM_IRQS];
    MemoryRegion *sysmem = get_system_memory();
    int n, myvirt_max_cpus;
#if 0
    MemoryRegion *ram = g_new(MemoryRegion, 1);
#endif
    MemoryRegion *rpm_ram = g_new(MemoryRegion, 1);

	if (!bios_name)
	{
		fprintf(stderr,"missing bios\n");
		exit(1);
	}

    /* We can probe only here because during property set
     * KVM is not available yet
     */
    if (!vms->gic_version) {
        if (!kvm_enabled()) {
            error_report("gic-version=host requires KVM");
            exit(1);
        }

        vms->gic_version = kvm_arm_vgic_probe();
        if (!vms->gic_version) {
            error_report("Unable to determine GIC version supported by host");
            exit(1);
        }
    }

    if (!cpu_type_valid(machine->cpu_type)) {
        error_report("mach-virt: CPU type %s not supported", machine->cpu_type);
        exit(1);
    }

	vms->psci_conduit = QEMU_PSCI_CONDUIT_DISABLED;

    /* The maximum number of CPUs depends on the GIC version, or on how
     * many redistributors we can fit into the memory map.
     */
    if (vms->gic_version == 3) {
        myvirt_max_cpus = vms->memmap[VIRT_GIC_REDIST].size / 0x20000;
    } else {
        myvirt_max_cpus = GIC_NCPU;
    }

    if (max_cpus > myvirt_max_cpus) {
        error_report("Number of SMP CPUs requested (%d) exceeds max CPUs "
                     "supported by machine 'mach-virt' (%d)",
                     max_cpus, myvirt_max_cpus);
        exit(1);
    }

    vms->smp_cpus = smp_cpus;

	if (vms->smp_cpus  != 1)
	{
		fprintf(stderr,"1 cpu\n");
		exit(1);
	}

    if (vms->virt && kvm_enabled()) {
        error_report("mach-virt: KVM does not support providing "
                     "Virtualization extensions to the guest CPU");
        exit(1);
    }

    if (vms->secure) {
		fprintf(stderr,"secure 2\n");
		exit(1);
    }

    create_fdt(vms);

    possible_cpus = mc->possible_cpu_arch_ids(machine);
    for (n = 0; n < possible_cpus->len; n++) {
        Object *cpuobj;
        CPUState *cs;

        if (n >= smp_cpus) {
            break;
        }

        cpuobj = object_new(machine->cpu_type);
        object_property_set_int(cpuobj, possible_cpus->cpus[n].arch_id,
                                "mp-affinity", NULL);

        cs = CPU(cpuobj);
        cs->cpu_index = n;

        numa_cpu_pre_plug(&possible_cpus->cpus[cs->cpu_index], DEVICE(cpuobj),
                          &error_fatal);

        if (!vms->secure) {
            object_property_set_bool(cpuobj, false, "has_el3", NULL);
        }

        if (!vms->virt && object_property_find(cpuobj, "has_el2", NULL)) {
            object_property_set_bool(cpuobj, false, "has_el2", NULL);
        }

        if (vms->psci_conduit != QEMU_PSCI_CONDUIT_DISABLED) {
            object_property_set_int(cpuobj, vms->psci_conduit,
                                    "psci-conduit", NULL);

            /* Secondary CPUs start in PSCI powered-down state */
            if (n > 0) {
                object_property_set_bool(cpuobj, true,
                                         "start-powered-off", NULL);
            }
        }

        if (vmc->no_pmu && object_property_find(cpuobj, "pmu", NULL)) {
            object_property_set_bool(cpuobj, false, "pmu", NULL);
        }

        if (object_property_find(cpuobj, "reset-cbar", NULL)) {
            object_property_set_int(cpuobj, vms->memmap[VIRT_CPUPERIPHS].base,
                                    "reset-cbar", &error_abort);
        }

        object_property_set_link(cpuobj, OBJECT(sysmem), "memory",
                                 &error_abort);
        object_property_set_bool(cpuobj, true, "realized", NULL);
        object_unref(cpuobj);
    }
    fdt_add_timer_nodes(vms);
    fdt_add_cpu_nodes(vms);
    fdt_add_psci_node(vms);

	fprintf(stderr,"RAM size: %d\n", machine->ram_size);

    create_gic(vms, pic);

    fdt_add_pmu_nodes(vms);

	/* Load ROM, create boot flash and pheripherals*/
    create_flash(vms, sysmem);
    memory_region_allocate_system_memory(rpm_ram, NULL, "mach-virt.rpm_ram",
                                        vms->memmap[VIRT_MSM_RPM_RAM].size );
    memory_region_add_subregion(sysmem, vms->memmap[VIRT_MSM_RPM_RAM].base, rpm_ram);
	create_msm_peripherals(vms, sysmem);

    create_rtc(vms, pic);

    vms->machine_done.notify = myvirt_machine_done;
    qemu_add_machine_init_done_notifier(&vms->machine_done);

	vms->bootinfo.ram_size = 0;
	vms->bootinfo.kernel_filename = NULL;
	vms->bootinfo.kernel_cmdline = NULL;
	vms->bootinfo.initrd_filename = NULL;
    vms->bootinfo.nb_cpus = 1;
    vms->bootinfo.board_id = -1;
    vms->bootinfo.loader_start = 0;
    vms->bootinfo.get_dtb = machmyvirt_dtb;
    vms->bootinfo.firmware_loaded = 0;

    /*
     * arm_load_kernel machine init done notifier registration must
     * happen before the platform_bus_create call. In this latter,
     * another notifier is registered which adds platform bus nodes.
     * Notifiers are executed in registration reverse order.
     */
    create_platform_bus(vms, pic);
}

static bool myvirt_get_secure(Object *obj, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    return vms->secure;
}

static void myvirt_set_secure(Object *obj, bool value, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    vms->secure = value;
}

static bool myvirt_get_virt(Object *obj, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    return vms->virt;
}

static void myvirt_set_virt(Object *obj, bool value, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    vms->virt = value;
}

static bool myvirt_get_highmem(Object *obj, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    return vms->highmem;
}

static void myvirt_set_highmem(Object *obj, bool value, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    vms->highmem = value;
}

static bool myvirt_get_its(Object *obj, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    return vms->its;
}

static void myvirt_set_its(Object *obj, bool value, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    vms->its = value;
}

static char *myvirt_get_gic_version(Object *obj, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);
    const char *val = vms->gic_version == 3 ? "3" : "2";

    return g_strdup(val);
}

static void myvirt_set_gic_version(Object *obj, const char *value, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    if (!strcmp(value, "3")) {
        vms->gic_version = 3;
    } else if (!strcmp(value, "2")) {
        vms->gic_version = 2;
    } else if (!strcmp(value, "host")) {
        vms->gic_version = 0; /* Will probe later */
    } else {
        error_setg(errp, "Invalid gic-version value");
        error_append_hint(errp, "Valid values are 3, 2, host.\n");
    }
}

static CpuInstanceProperties
myvirt_cpu_index_to_props(MachineState *ms, unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static int64_t myvirt_get_default_cpu_node_id(const MachineState *ms, int idx)
{
    return idx % nb_numa_nodes;
}

static const CPUArchIdList *myvirt_possible_cpu_arch_ids(MachineState *ms)
{
    int n;
    VirtMachineState *vms = VIRT_MACHINE(ms);

    if (ms->possible_cpus) {
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (n = 0; n < ms->possible_cpus->len; n++) {
        ms->possible_cpus->cpus[n].arch_id =
            myvirt_cpu_mp_affinity(vms, n);
        ms->possible_cpus->cpus[n].props.has_thread_id = true;
        ms->possible_cpus->cpus[n].props.thread_id = n;
    }
    return ms->possible_cpus;
}

static void myvirt_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = machmyvirt_init;
    /* Start max_cpus at the maximum QEMU supports. We'll further restrict
     * it later in machmyvirt_init, where we have more information about the
     * configuration of the particular instance.
     */
    mc->max_cpus = 255;
    mc->has_dynamic_sysbus = true;
    mc->block_default_type = IF_VIRTIO;
    mc->no_cdrom = 1;
    mc->pci_allow_0_address = true;
    /* We know we will never create a pre-ARMv7 CPU which needs 1K pages */
    mc->minimum_page_bits = 12;
    mc->possible_cpu_arch_ids = myvirt_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = myvirt_cpu_index_to_props;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a15");
    mc->get_default_cpu_node_id = myvirt_get_default_cpu_node_id;
}

static const TypeInfo myvirt_machine_info = {
    .name          = TYPE_VIRT_MACHINE,
    .parent        = TYPE_MACHINE,
    .abstract      = true,
    .instance_size = sizeof(VirtMachineState),
    .class_size    = sizeof(VirtMachineClass),
    .class_init    = myvirt_machine_class_init,
};

static void machmyvirt_machine_init(void)
{
    type_register_static(&myvirt_machine_info);
}
type_init(machmyvirt_machine_init);

static void myvirt_2_10_instance_init(Object *obj)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);
    VirtMachineClass *vmc = VIRT_MACHINE_GET_CLASS(vms);

    /* EL3 is disabled by default on virt: this makes us consistent
     * between KVM and TCG for this board, and it also allows us to
     * boot UEFI blobs which assume no TrustZone support.
     */
    vms->secure = false;
    object_property_add_bool(obj, "secure", myvirt_get_secure,
                             myvirt_set_secure, NULL);
    object_property_set_description(obj, "secure",
                                    "Set on/off to enable/disable the ARM "
                                    "Security Extensions (TrustZone)",
                                    NULL);

    /* EL2 is also disabled by default, for similar reasons */
    vms->virt = false;
    object_property_add_bool(obj, "virtualization", myvirt_get_virt,
                             myvirt_set_virt, NULL);
    object_property_set_description(obj, "virtualization",
                                    "Set on/off to enable/disable emulating a "
                                    "guest CPU which implements the ARM "
                                    "Virtualization Extensions",
                                    NULL);

    /* High memory is enabled by default */
    vms->highmem = true;
    object_property_add_bool(obj, "highmem", myvirt_get_highmem,
                             myvirt_set_highmem, NULL);
    object_property_set_description(obj, "highmem",
                                    "Set on/off to enable/disable using "
                                    "physical address space above 32 bits",
                                    NULL);
    /* Default GIC type is v2 */
    vms->gic_version = 2;
    object_property_add_str(obj, "gic-version", myvirt_get_gic_version,
                        myvirt_set_gic_version, NULL);
    object_property_set_description(obj, "gic-version",
                                    "Set GIC version. "
                                    "Valid values are 2, 3 and host", NULL);

    if (vmc->no_its) {
        vms->its = false;
    } else {
        /* Default allows ITS instantiation */
        vms->its = true;
        object_property_add_bool(obj, "its", myvirt_get_its,
                                 myvirt_set_its, NULL);
        object_property_set_description(obj, "its",
                                        "Set on/off to enable/disable "
                                        "ITS instantiation",
                                        NULL);
    }

    vms->memmap = msm_memmap;
    vms->irqmap = msm_irqmap;
}

static void myvirt_machine_2_10_options(MachineClass *mc)
{
}
DEFINE_VIRT_MACHINE_AS_LATEST(2, 10)

#define VIRT_COMPAT_2_9 \
    HW_COMPAT_2_9

