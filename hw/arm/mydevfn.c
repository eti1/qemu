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
#include <openssl/sha.h>

#include "my_dev.h"

#define MMCFILE "dmp/mmc"

void printregs(void);
static void write_sdc1_cmd(unsigned value);

static int rpm_delay_state = 0;

static uint64_t sdc_status = 0;
static unsigned int sdc_cmd_arg = 0;

static unsigned sdc_resp[4] = {0};
static unsigned sdc_blocklen = 0x200;
static unsigned sdc_datalen = 0;

static unsigned char sdc_fifo_buf[0x4C000];
static unsigned char *sdc_fifo_data = NULL;
static unsigned int sdc_fifo_pos = 0;
static unsigned int sdc_fifo_size = 0;

static SHA_CTX sha_ctx;
static SHA256_CTX sha2_ctx;
static unsigned int sha_size = 0;
static unsigned int sha_cfg = 0;
static unsigned int sha_cnt = 0;
static unsigned int sha_state = 0;
static unsigned cmd8_state = 0;
unsigned char sha_hash[32];

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
	static uint32_t val = 0;

	printregs();
	switch(offset)
	{
	case 0x2004:
		/* Elapsed timer :::::::*/
		switch(rpm_delay_state)
		{
		case 0:
			rpm_delay_state = 1;
			v = 0;
			val = 0;
			break;
		case 1:
			v = val;
			val += 8;
			break;
		default:
			break;
		}
		break;
	default:
		fprintf(stderr,"%6x: dev rpm(): READ (%d) at %s", 
				ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(rpm, offset));
		v = 0;
		fprintf(stderr, " -> %8x\n", (unsigned)v);
		break;
	}

    return v;
}

static unsigned int sdc_read_status(void)
{
	unsigned v;
	unsigned avail = (sdc_fifo_size - sdc_fifo_pos);

	v = sdc_status;
	if (avail)
	{
		v |= 1<<21; // rx available
	}
	if (avail >= 16)
	{
		v |= 1<<15; // rxfifo half full
	}
	if (avail >= 32)
	{
		v |= 1<<17; // rxfifo full
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
	case 0x38:
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
		v = sdc_resp[ridx];
		fprintf(stderr, "\nsdc read_response(%d) -> %08x\n", ridx, v);
		break;
	case 0x34: // status
		//fprintf(stderr,"%6x: dev sdc(): READ (%d) at %s\n", 
		//		ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(sdc1, offset));
		fprintf(stderr, "?");
		fflush(stderr);
		v = sdc_read_status();
		//fprintf(stderr, "SDC Status: %08x\n", v);
		break;
	case 0x80: case 0x84: case 0x88: case 0x8c:
	case 0x90: case 0x94: case 0x98: case 0x9c:
		fprintf(stderr, ">");
		//fprintf(stderr,"%6x: dev sdc(): READ fifo", 
		//		ARM_CPU(qemu_get_cpu(0))->env.regs[15]);
		v = read_sdc_fifo();
		//fprintf(stderr, " -> %8x\n", v);
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
			sdc_resp[0] = 0xFEFFFF30;
			sdc_resp[1] = '1234';
			sdc_resp[2] = 0x35fFFFFF;
			sdc_resp[3] = 0xFFFFFFFF;
			break;
		case 3:
			fprintf(stderr, "CMD3: SEND_RELATIVE_ADDRESS\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0;
			break;
		case 6:
			fprintf(stderr, "CMD6: ???\n");
			sdc_status = 0x81;
			sdc_resp[0] = 0;
			break;
		case 7:
			fprintf(stderr, "CMD7: SELECT/DESELECT_CARD\n");
			sdc_status = 0x40;
			sdc_resp[0] = 0;
			break;
		case 8:
			fprintf(stderr, "CMD8: SEND_IF_CONF\n");
			if (cmd8_state == 0)
			{
				sdc_status = 0x40;
				cmd8_state = 1;
			}
			else
			{
				sdc_status = 0x81;
			}
			sdc_resp[0] = 0x1aa;
			break;
		case 9:
			fprintf(stderr, "CMD9: SEND_CSD\n");
			sdc_status = 0x40;
			//sdc_resp[0] = 0x80000000;
			//sdc_resp[1] = 0x00090000 ;
			//sdc_resp[2] = 0x19000000 ;
#if 0
			sdc_resp[0]=( 0x80000000	// ?
						| 0x10000000); // ? reserved = 4
			sdc_resp[1] = 0x000F00cf ;
			sdc_resp[2] = 0xd9010000 ;
			sdc_resp[3] = 0 ;
#else
			sdc_resp[0]=( 0x80000000	// ?
						| 0x10000000); // ? reserved = 4
			sdc_resp[1] = 0x000F00cf ;
			sdc_resp[2] = 0xd9038000 ;
			sdc_resp[3] = 0 ;
#endif
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
		case 23:
			fprintf(stderr, "CMD%d: SET_ERASE_COUNT: 0x%x\n", c, sdc_cmd_arg);
			sdc_status = 0x40;
			sdc_resp[0] = 0;
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

//    fprintf(stderr,"%6x: dev coproc(): READ (%d) at %s", 
//			ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(coproc, offset));
	switch(offset)
	{
	case 8:
		
	case 0x100:
		v = 0x1C06;
		break;
	default:
		break;
	}
	if (offset >= 0x450 && offset < 0x470)
	{
		v = *(unsigned*)&sha_hash[offset-0x450];
	}
//	fprintf(stderr, " -> %8x\n", (unsigned)v);

    return v;
}

void devfn_coproc_write(void *vms_, hwaddr offset,
                         uint64_t value, unsigned size)
{
	unsigned char b[4];
	unsigned n,i,v,sz;

//    fprintf(stderr,"%6x: dev coproc(): WRITE(%d) at %s -> %8x\n",
//			ARM_CPU(qemu_get_cpu(0))->env.regs[15],
//			size, FINDREG(coproc,offset),(unsigned)value );
	switch(offset)
	{
	case 8:
		if (sha_size && sha_state)
		{
			n = sha_size;
			if (n > 4) n = 4;
			b[0]=0xff&(value>>24);
			b[1]=0xff&(value>>16);
			b[2]=0xff&(value>>8);
			b[3]=0xff&(value>>0);
//			fprintf(stderr, "(update %d/%d: ",n,sha_size);
			//for(i=0;i<n;i++)fprintf(stderr,"%02x ", b[i]);
			if (sha_state == 2)
			{
				SHA256_Update(&sha2_ctx, &b, n);
			}
			else
			{
				SHA1_Update(&sha_ctx, &b, n);
			}
			sha_size -= n;
			sha_cnt += n;
			if (!sha_size && (sha_cfg & 0x4000))
			{
				fprintf(stderr, "(SHA final %d)\n",sha_cnt);
				if (sha_state == 2)
				{
					SHA256_Final(sha_hash, &sha2_ctx);
					sz = 32;
				}
				else{
					SHA1_Final(sha_hash, &sha_ctx);
					sz = 20;
				}
				for (i=0;i<sz;i+=4)
				{
					v = sha_hash[i];
					sha_hash[i] = sha_hash[i+3];
					sha_hash[i+3] = v;
					v = sha_hash[i+1];
					sha_hash[i+1] = sha_hash[i+2];
					sha_hash[i+2] = v;
				}
				sha_state = 0;
			}
		}
		break;
	case 0x400:
		sha_cfg = value;
		if (sha_state == 0)
		{
			sha_cnt = 0;
			if (sha_cfg & 0x200)
			{
				fprintf(stderr, "(SHA2 init)\n");
				SHA256_Init(&sha2_ctx);
				sha_state = 2;
			}
			else
			{
				fprintf(stderr, "(SHA1 init)\n");
				SHA1_Init(&sha_ctx);
				sha_state = 1;
			}
		}
		break;
	case 0x404:
		sha_size = value;
		break;
	default:
		break;
	}
}

uint64_t devfn_uart_read(void *vms_, hwaddr offset,
                            unsigned size)
{
	uint64_t v = 0;

#if 0
    fprintf(stderr,"%6x: dev uart(1a040000): READ (%d) at %s", 
			ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(uart, offset));
#endif
	switch(offset)
	{
	case 0x8:
		/* SR: TXRDY */
		v = 0x4;
		break;
	case 0x14:
		/* ISR: TX_READY */
		v = 0x80;
		break;
	default:
		break;
	}
//	fprintf(stderr, " -> %8x\n", (unsigned)v);

    return v;
}

void devfn_uart_write(void *vms_, hwaddr offset,
                         uint64_t value, unsigned size)
{
	static unsigned ccnt = 0;
	char c;

	switch(offset)
	{
	case 0x70:
		c = (char)value;
		if (!ccnt)
		{
			fprintf(stderr, "[UART] ");
		}
		fprintf(stderr, "%c",c);
		ccnt++;
		if (c == '\n')
		{
			ccnt = 0;
		}
	}
#if 0
    fprintf(stderr,"%6x: dev uart(1a040000): WRITE(%d) at %s -> %8x\n",
			ARM_CPU(qemu_get_cpu(0))->env.regs[15],
			size, FINDREG(uart,offset),(unsigned)value );
#endif
}

uint64_t devfn_clk_ctl_read(void *vms_, hwaddr offset,
                            unsigned size)
{
	uint64_t v = 0;

    fprintf(stderr,"%6x: dev clk_ctl(00900000): READ (%d) at %s", 
			ARM_CPU(qemu_get_cpu(0))->env.regs[15], size, FINDREG(clk_ctl, offset));
	switch(offset)
	{
	case 0x3420:
		v = 0x600;
		break;
	default:
		break;
	}
	fprintf(stderr, " -> %8x\n", (unsigned)v);

    return v;
}
