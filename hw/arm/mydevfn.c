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

#include "my_dev.h"

void printregs(void)
{
	int i;

#if 0
	for(i=0;i<16;i++)
	{
		fprintf(stderr,"r%d: %8x\n",i,ARM_CPU(qemu_get_cpu(0))->env.regs[i]);
	}
#endif
}

uint64_t devfn_sec_ctrl_read(void *vms_, hwaddr offset,
                            unsigned size)
{
	uint64_t v = 0;

	printregs();
    fprintf(stderr,"%6x: dev sec_ctrl(00700000): READ (%d) at %s", 
			ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(sec_ctrl, offset));

	switch(offset)
	{
	case 0x6034:
		/* Start from ROM*/
		v = 1;
		break;
	default:
		break;
	}

	fprintf(stderr, " -> %8x\n", (unsigned)v);

    return v;
}

static int rpm_delay_state = 0;
void devfn_rpm_write(void *vms_, hwaddr offset,
                         uint64_t value, unsigned size)
{
    fprintf(stderr,"%6x: dev rpm(00060000): WRITE(%d) at %s -> %8x\n",
			ARM_CPU(qemu_get_cpu(0))->env.regs[15],
			size, FINDREG(rpm,offset),(unsigned)value );

	printregs();
	switch(offset)
	{
	case 0x200c:
		rpm_delay_state = 0;
		break;
	default:
		break;
	}
}

uint64_t devfn_rpm_read(void *vms_, hwaddr offset,
                            unsigned size)
{
	uint64_t v = 0;

	printregs();
    fprintf(stderr,"%6x: dev rpm(00060000): READ (%d) at %s", 
			ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(rpm, offset));
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
	fprintf(stderr, " -> %8x\n", (unsigned)v);

    return v;
}

uint64_t devfn_sdc3_read(void *vms_, hwaddr offset,
                            unsigned size)
{
	uint64_t v = 0;

	printregs();
    fprintf(stderr,"%6x: dev sdc3(12180000): READ (%d) at %s", 
			ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(sdc3, offset));

	switch(offset)
	{
	case 0:
		break;
	default:
		break;
	}
	fprintf(stderr, " -> %8x\n", (unsigned)v);

    return v;
}

void devfn_sdc3_write(void *vms_, hwaddr offset,
                         uint64_t value, unsigned size)
{
	printregs();
    fprintf(stderr,"%6x: dev sdc3(12180000): WRITE(%d) at %s -> %8x\n",
			ARM_CPU(qemu_get_cpu(0))->env.regs[15],
			size, FINDREG(sdc3,offset),(unsigned)value );

	switch(offset)
	{
	case 0:
		break;
	default:
		break;
	}
}

