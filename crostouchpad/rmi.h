#include <stdint.h>
#include "linuxmacros.h"

#ifndef __packed
#define __packed( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop) )
#endif

#define RMI_MOUSE_REPORT_ID		0x01 /* Mouse emulation Report */
#define RMI_WRITE_REPORT_ID		0x09 /* Output Report */
#define RMI_READ_ADDR_REPORT_ID		0x0a /* Output Report */
#define RMI_READ_DATA_REPORT_ID		0x0b /* Input Report */
#define RMI_ATTN_REPORT_ID		0x0c /* Input Report */
#define RMI_SET_RMI_MODE_REPORT_ID	0x0f /* Feature Report */

/* flags */
#define RMI_READ_REQUEST_PENDING	0
#define RMI_READ_DATA_PENDING		1
#define RMI_STARTED			2

#define RMI_SLEEP_NORMAL		0x0
#define RMI_SLEEP_DEEP_SLEEP		0x1

/* device flags */
#define RMI_DEVICE			BIT(0)
#define RMI_DEVICE_HAS_PHYS_BUTTONS	BIT(1)

/*
* retrieve the ctrl registers
* the ctrl register has a size of 20 but a fw bug split it into 16 + 4,
* and there is no way to know if the first 20 bytes are here or not.
* We use only the first 12 bytes, so get only them.
*/
#define RMI_F11_CTRL_REG_COUNT		12

enum rmi_mode_type {
	RMI_MODE_OFF = 0,
	RMI_MODE_ATTN_REPORTS = 1,
	RMI_MODE_NO_PACKED_ATTN_REPORTS = 2,
};

struct rmi_function {
	unsigned page;			/* page of the function */
	uint16_t query_base_addr;		/* base address for queries */
	uint16_t command_base_addr;		/* base address for commands */
	uint16_t control_base_addr;		/* base address for controls */
	uint16_t data_base_addr;		/* base address for datas */
	unsigned int interrupt_base;	/* cross-function interrupt number
									* (uniq in the device)*/
	unsigned int interrupt_count;	/* number of interrupts */
	unsigned int report_size;	/* size of a report */
	unsigned long irq_mask;		/* mask of the interrupts
								* (to be applied against ATTN IRQ) */
};

#define RMI_PAGE(addr) (((addr) >> 8) & 0xff)

#define RMI4_MAX_PAGE 0xff
#define RMI4_PAGE_SIZE 0x0100

#define PDT_START_SCAN_LOCATION 0x00e9
#define PDT_END_SCAN_LOCATION	0x0005
#define RMI4_END_OF_PDT(id) ((id) == 0x00 || (id) == 0xff)

__packed(struct pdt_entry {
	uint8_t query_base_addr : 8;
	uint8_t command_base_addr : 8;
	uint8_t control_base_addr : 8;
	uint8_t data_base_addr : 8;
	uint8_t interrupt_source_count : 3;
	uint8_t bits3and4 : 2;
	uint8_t function_version : 2;
	uint8_t bit7 : 1;
	uint8_t function_number : 8;
});

#define RMI_DEVICE_F01_BASIC_QUERY_LEN	11

#define MXT_T9_RELEASE		(1 << 5)
#define MXT_T9_PRESS		(1 << 6)
#define MXT_T9_DETECT		(1 << 7)

struct touch_softc {
	int *x;
	int *y;
	int *p;
	int *palm;
};