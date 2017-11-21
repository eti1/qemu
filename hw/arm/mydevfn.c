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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "my_dev.h"

void printregs(void);
static void write_sdc1_cmd(unsigned value);

void printregs(void)
{
#if 0
	int i;

	for(i=0;i<16;i++)
	{
		fprintf(stderr,"r%d: %8x\n",i,ARM_CPU(qemu_get_cpu(0))->env.regs[i]);
	}
#endif
}

uint64_t devfn_secure_read(void *vms_, hwaddr offset,
                            unsigned size)
{
	uint64_t v = 0;

	printregs();
    fprintf(stderr,"%6x: dev sec(): READ (%d) at %s", 
			ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(secure, offset));

	switch(offset)
	{
	case 0x6000: v = 0x00000000; break;
	case 0x6004: v = 0x032000a1; break;
	case 0x6008: v = 0x00000000; break;
	case 0x600c: v = 0x7e400001; break;
	case 0x6010: v = 0x00005d67; break;
	case 0x6014: v = 0x00201000; break;
	case 0x6018: v = 0x00000000; break;
	case 0x601c: v = 0x00000086; break;
	case 0x6020: v = 0x00006000; break;
	case 0x6024: v = 0x9bfc000e; break;
	case 0x6028: v = 0x02003800; break;
	case 0x602c: v = 0x00010000; break;
	case 0x6030: v = 0x00000001; break;
	case 0x6034: v = 0x0000000f; break;
	case 0x6038: v = 0x00000125; break;
	case 0x603c: v = 0x00000100; break;
	case 0x60a8: v = 0x00000000; break;
	case 0x60c8: v = 0x00000043; break;
	case 0x60cc: v = 0x00200008; break;
	case 0x60d0: v = 0x00000000; break;
	case 0x60d8: v = 0x217f214b; break;
	case 0x60dc: v = 0x0000008f; break;
	case 0x60e0: v = 0x107e50e1; break;
	case 0x60e4: v = 0x00010001; break;
	case 0x60e8: v = 0x00000000; break;
	case 0x60ec: v = 0x00000001; break;
	case 0x60f0: v = 0x00000000; break;
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
    fprintf(stderr,"%6x: dev rpm(): WRITE(%d) at %s -> %8x\n",
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
    fprintf(stderr,"%6x: dev rpm(): READ (%d) at %s", 
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
			v = 0x7fffffff;
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

static uint64_t sdc_status = 0;
static unsigned int sdc_cmd_arg = 0;

static unsigned sdc_resp[4] = {0};
static unsigned sdc_blocklen = 0x200;
static unsigned sdc_datalen = 0;

static unsigned char sdc_fifo_buf[0x40000];
static unsigned char *sdc_fifo_data = NULL;
static unsigned int sdc_fifo_pos = 0;
static unsigned int sdc_fifo_size = 0;

static unsigned int sdc_read_status(void)
{
	unsigned v;

	v = sdc_status;
	if (sdc_fifo_pos < sdc_fifo_size)
	{
		v |= 0x200000;
	}
	sdc_status = 0;

	return v;
}

static void sdc_init_fifo(void *data, unsigned int size)
{
	sdc_fifo_data = data;
	sdc_fifo_size = size;
	sdc_fifo_pos = 0;
}

static unsigned read_sdc_fifo(void)
{
	unsigned i, v=0;

	// fprintf(stderr, " - READ_FIFO(%u/%u) - ", sdc_fifo_pos, sdc_fifo_size);
	for(i=0; i<4; i++)
	{
		if (i+sdc_fifo_pos < sdc_fifo_size)
		{
			((unsigned char*)&v)[i] = sdc_fifo_data[sdc_fifo_pos+i];
		}
	}
	sdc_fifo_pos += 4;
	if (sdc_fifo_pos >= sdc_fifo_size)
	{
		fprintf(stderr,"read_sdc_fifo done\n");
		sdc_fifo_pos = sdc_fifo_size = 0;
	}
	return v;
}

#define MMCFILE "dmp/mmc"

static void sdc_read_block(unsigned addr, unsigned size)
{
	static int fd = -1;

	if (fd == -1)
	{
		if (-1 == (fd = open(MMCFILE, O_RDONLY)))
		{
			fprintf(stderr,"File " MMCFILE " not found\n");
			exit(1);
		}
	}
	fprintf(stderr, "sdc_read(at 0x%x, size 0x%x)\n",addr,size);
	if ((addr|size) & 0x1FF || size > sizeof(sdc_fifo_buf))
	{
		fprintf(stderr, "Invalid arguments\n");
		exit(1);
	}
	if (lseek(fd, addr, SEEK_SET) == -1 || size != read(fd, sdc_fifo_buf, size))
	{
		fprintf(stderr, "Failed to read block\n");
		exit(1);
	}
	sdc_init_fifo(sdc_fifo_buf, size);
}

void devfn_sdc1_write(void *vms_, hwaddr offset,
                         uint64_t value, unsigned size)
{
	printregs();
    fprintf(stderr,"%6x: dev sdc(): WRITE(%d) at %s -> %8x\n",
			ARM_CPU(qemu_get_cpu(0))->env.regs[15],
			size, FINDREG(sdc1,offset),(unsigned)value );

	switch(offset)
	{
	case 0:
		break;
	case 8:
		sdc_cmd_arg = ((unsigned)value);
		break;
	case 0xc:
		write_sdc1_cmd((unsigned)value);
		break;
	case 0x28:
		sdc_datalen = (unsigned)value;
		break;
	default:
		break;
	}
}

uint64_t devfn_sdc1_read(void *vms_, hwaddr offset,
                            unsigned size)
{
	unsigned int v = 0;
	unsigned int ridx;

	printregs();
	switch(offset)
	{
	case 0x14: case 0x18: case 0x1C: case 0x20:
		fprintf(stderr,"%6x: dev sdc(): READ (%d) at %s", 
				ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(sdc1, offset));
		ridx = (offset-0x14)>>2;
		fprintf(stderr, "\nsdc read_response(%d)\n", ridx);
		v = sdc_resp[ridx];
		break;
	case 0x34: // status
		//fprintf(stderr,"%6x: dev sdc(): READ (%d) at %s\n", 
		//		ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(sdc1, offset));
		fprintf(stderr, "?");
		fflush(stderr);
		v = sdc_read_status();
		break;
	case 0x80: case 0x84: case 0x88: case 0x8c:
	case 0x90: case 0x94: case 0x98: case 0x9c:
		fprintf(stderr, "+");
		v = read_sdc_fifo();
		break;
	default:
		fprintf(stderr,"%6x: dev sdc(): READ (%d) at %s", 
				ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(sdc1, offset));
		fprintf(stderr, " -> %8x\n", v);
		break;
	}

    return (uint64_t)v;
}

static void write_sdc1_cmd(unsigned value)
{
	static int c13_state = 0;
	unsigned c,e,r;

	c = (value) & 0x3f;
	r = (value>>6) & 3;
	e = (value>>10) & 1;

	if (e & 0xfffffb00)
	{
		fprintf(stderr, "unknown sdc cmd: %08x\n", value);
		exit(1);
	}

	if (e)
	{
		switch(c)
		{
		case 0:
			fprintf(stderr,"CMD0: GO_IDLE_STATE\n");
			sdc_status = 0x80; // end
			break;
		case 1:
			fprintf(stderr,"CMD1: SEND_OP_COND\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0xA0000000;
			break;
		case 2:
			fprintf(stderr, "CMD2: ALL_SEND_CID\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0;
			break;
		case 3:
			fprintf(stderr, "CMD3: SEND_RELATIVE_ADDRESS\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0;
			break;
		case 7:
			fprintf(stderr, "CMD7: SELECT/DESELECT_CARD\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0;
			break;
		case 8:
			fprintf(stderr, "CMD8: SEND_IF_CONF\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0x1aa;
			break;
		case 9:
			fprintf(stderr, "CMD9: SEND_CSD\n");
			sdc_status = 0x40;
			//sdc_resp[0] = 0x80000000;
			//sdc_resp[1] = 0x00090000 ;
			//sdc_resp[2] = 0x19000000 ;
			sdc_resp[0] = 0x80000000;
			sdc_resp[1] = 0x000F0000 ;
			sdc_resp[2] = 0x19000000 ;
			sdc_resp[3] = 0 ;
			break;
		case 12:
			fprintf(stderr, "CMD12: STOP_TRANSMISSION\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0;
			break;
		case 13:
			fprintf(stderr, "CMD13: SEND_STATUS\n");
			sdc_status = 0x40;
			switch(c13_state){
			case 0:
				sdc_resp[0] = (3<<9);
				c13_state = 1;
				break;
			default:
				sdc_resp[0] = (4<<9);
				break;
			}
			break;
		case 16:
			fprintf(stderr, "CMD16: SET_BLOCKLEN\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0;
			sdc_blocklen = sdc_cmd_arg;
			break;
		case 17: case 18:
			fprintf(stderr, "CMD%d: READ_%s_BLOCK(at 0x%x size 0x%x)\n",
				c, c==17?"SINGLE":"MULTIPLE", sdc_cmd_arg, sdc_datalen);
			sdc_status = 0x40;
			sdc_resp[0] = 0;
			sdc_read_block(sdc_cmd_arg, sdc_datalen);
			break;
		case 41:
			fprintf(stderr, "ACMD41: SD_SEND_OP_COND\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0x80000000;
			break;
		case 51:
			fprintf(stderr, "ACMD51: SD_SEND_SCR\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0;
			sdc_fifo_buf[0] = 1;
			sdc_fifo_buf[1] = 1;
			memset(sdc_fifo_buf+2,0,6);
			sdc_init_fifo(sdc_fifo_buf, 8);
			break;
		case 55:
			fprintf(stderr, "CMD55: APP_CMD\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0;
			break;
		default:
			fprintf(stderr,"cmd %d, rtype %d\n",c,r);
			break;
		}
	}
}

uint64_t devfn_pmc_cmd_read(void *vms_, hwaddr offset,
                            unsigned size)
{
	uint64_t v = 0;

    fprintf(stderr,"%6x: dev pmc_cmd(): READ (%d) at %s", 
			ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(pmc_cmd, offset));
	switch(offset)
	{
	case 4:
		v = 0x8000000;
		break;
	default:
		break;
	}
	fprintf(stderr, " -> %8x\n", (unsigned)v);

    return v;
}


uint64_t devfn_coproc_read(void *vms_, hwaddr offset,
                            unsigned size)
{
	uint64_t v = 0;

    fprintf(stderr,"%6x: dev coproc(): READ (%d) at %s", 
			ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(coproc, offset));
	switch(offset)
	{
	case 0x100:
		v = 0x1C06;
		break;
	default:
		break;
	}
	fprintf(stderr, " -> %8x\n", (unsigned)v);

    return v;
}

void devfn_coproc_write(void *vms_, hwaddr offset,
                         uint64_t value, unsigned size)
{
    fprintf(stderr,"%6x: dev coproc(): WRITE(%d) at %s -> %8x\n",
			ARM_CPU(qemu_get_cpu(0))->env.regs[15],
			size, FINDREG(coproc,offset),(unsigned)value );
}
