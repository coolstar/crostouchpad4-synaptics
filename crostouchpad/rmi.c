#include "driver.h"

static ULONG SynaPrintDebugLevel = 100;
static ULONG SynaPrintDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

static NTSTATUS rmi_set_mode(PSYNA_CONTEXT pDevice, uint8_t mode) {
	uint8_t command[] = { 0x00, 0x3f, 0x03, 0x0f, 0x23, 0x00, 0x04, 0x00, RMI_SET_RMI_MODE_REPORT_ID, mode }; //magic bytes from Linux
	return SpbWriteDataSynchronously(&pDevice->I2CContext, 0x22, command, sizeof(command));
}

static NTSTATUS rmi_write_report(PSYNA_CONTEXT pDevice, uint8_t *report, size_t report_size) {
	uint8_t command[24];
	command[0] = 0x00;
	command[1] = 0x17;
	command[2] = 0x00;
	//SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "RMI Write Report: ");
	for (int i = 0; i < report_size; i++) {
		//SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "0x%x ", report[i]);
		command[i + 3] = report[i];
	}
	//SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "\n");
	return SpbWriteDataSynchronously(&pDevice->I2CContext, 0x25, command, sizeof(command));
}

static NTSTATUS rmi_set_page(PSYNA_CONTEXT pDevice, uint8_t page)
{
	uint8_t writeReport[21];
	NTSTATUS retval;

	writeReport[0] = RMI_WRITE_REPORT_ID;
	writeReport[1] = 1;
	writeReport[2] = 0xFF;
	writeReport[4] = page;

	SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Set Page\n");

	retval = rmi_write_report(pDevice, writeReport,
		sizeof(writeReport));
	SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Report Written\n");

	pDevice->page = page;
	return retval;
}

static NTSTATUS rmi_read_block(PSYNA_CONTEXT pDevice, uint16_t addr, uint8_t *buf,
	const int len)
{
	SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Read Block: 0x%x\n", addr);
	NTSTATUS status = 0;
	int bytes_read;
	int bytes_needed;
	int retries;
	int read_input_count;

	if (RMI_PAGE(addr) != pDevice->page) {
		status = rmi_set_page(pDevice, RMI_PAGE(addr));
		if (!NT_SUCCESS(status))
			goto exit;
	}

	uint8_t writeReport[21];
	for (int i = 0; i < 21; i++) {
		writeReport[i] = 0;
	}
	writeReport[0] = RMI_READ_ADDR_REPORT_ID;
	writeReport[1] = 0;
	writeReport[2] = addr & 0xFF;
	writeReport[3] = (addr >> 8) & 0xFF;
	writeReport[4] = len & 0xFF;
	writeReport[5] = (len >> 8) & 0xFF;
	status = rmi_write_report(pDevice, writeReport, sizeof(writeReport));
	if (!NT_SUCCESS(status))
		goto exit;

	uint8_t i2cInput[42];
	status = SpbOnlyReadDataSynchronously(&pDevice->I2CContext, i2cInput, sizeof(i2cInput));
	if (!NT_SUCCESS(status))
		goto exit;

	uint8_t rmiInput[40];
	for (int i = 0; i < 40; i++) {
		rmiInput[i] = i2cInput[i + 2];
	}
	SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "RMI Input ID: 0x%x\n", rmiInput[0]);
	if (rmiInput[0] == RMI_READ_DATA_REPORT_ID) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "RMI Read: ");
		for (int i = 0; i < len; i++) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "0x%x ", rmiInput[i + 2]);
			buf[i] = rmiInput[i + 2];
		}
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "\n");
	}
exit:
	return status;
}

static NTSTATUS rmi_read(PSYNA_CONTEXT pDevice, uint16_t addr, uint8_t *buf) {
	return rmi_read_block(pDevice, addr, buf, 1);
}

static NTSTATUS rmi_write_block(PSYNA_CONTEXT pDevice, uint16_t addr, uint8_t *buf, const int len)
{
	NTSTATUS status = STATUS_SUCCESS;

	uint8_t writeReport[21];
	for (int i = 0; i < 21; i++) {
		writeReport[i] = 0;
	}

	if (RMI_PAGE(addr) != pDevice->page) {
		status = rmi_set_page(pDevice, RMI_PAGE(addr));
		if (!NT_SUCCESS(status))
			goto exit;
	}

	writeReport[0] = RMI_WRITE_REPORT_ID;
	writeReport[1] = len;
	writeReport[2] = addr & 0xFF;
	writeReport[3] = (addr >> 8) & 0xFF;
	for (int i = 0; i < len; i++) {
		writeReport[i + 4] = buf[i];
	}

	status = rmi_write_report(pDevice, writeReport, sizeof(writeReport));
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "failed to write request output report (%d)\n",
			status);
		goto exit;
	}

exit:
	return status;
}

static NTSTATUS rmi_write(PSYNA_CONTEXT pDevice, uint16_t addr, uint8_t *buf)
{
	return rmi_write_block(pDevice, addr, buf, 1);
}

static unsigned long rmi_gen_mask(unsigned irq_base, unsigned irq_count)
{
	return GENMASK(irq_count + irq_base - 1, irq_base);
}

static void rmi_register_function(PSYNA_CONTEXT pDevice, struct pdt_entry *pdt_entry, int page, unsigned interrupt_count)
{
	struct rmi_function *f = NULL;
	uint16_t page_base = page << 8;

	switch (pdt_entry->function_number) {
	case 0x01:
		f = &pDevice->f01;
		break;
	case 0x11:
		f = &pDevice->f11;
		break;
	case 0x30:
		f = &pDevice->f30;
		break;
	}

	if (f) {
		f->page = page;
		f->query_base_addr = page_base | pdt_entry->query_base_addr;
		f->command_base_addr = page_base | pdt_entry->command_base_addr;
		f->control_base_addr = page_base | pdt_entry->control_base_addr;
		f->data_base_addr = page_base | pdt_entry->data_base_addr;
		f->interrupt_base = interrupt_count;
		f->interrupt_count = pdt_entry->interrupt_source_count;
		f->irq_mask = rmi_gen_mask(f->interrupt_base,
			f->interrupt_count);
		pDevice->interrupt_enable_mask |= f->irq_mask;
	}
}

int rmi_scan_pdt(PSYNA_CONTEXT pDevice)
{
	struct pdt_entry entry;
	int page;
	bool page_has_function;
	int i;
	int retval;
	int interrupt = 0;
	uint16_t page_start, pdt_start, pdt_end;

	SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Scanning PDT...\n");

	for (page = 0; (page <= RMI4_MAX_PAGE); page++) {
		page_start = RMI4_PAGE_SIZE * page;
		pdt_start = page_start + PDT_START_SCAN_LOCATION;
		pdt_end = page_start + PDT_END_SCAN_LOCATION;

		page_has_function = false;
		for (i = pdt_start; i >= pdt_end; i -= sizeof(entry)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Read PDT Entry...\n");
			retval = rmi_read_block(pDevice, i, (uint8_t *)&entry, sizeof(entry));
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Read PDT Entry done\n");
			if (retval) {
				SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Read of PDT entry at %#06x failed.\n",
					i);
				goto error_exit;
			}

			if (RMI4_END_OF_PDT(entry.function_number))
				break;

			page_has_function = true;

			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Found F%02X on page %#04x\n",
				entry.function_number, page);

			rmi_register_function(pDevice, &entry, page, interrupt);
			interrupt += entry.interrupt_source_count;
		}

		if (!page_has_function)
			break;
	}

	SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Done with PDT scan.\n");
	retval = 0;

error_exit:
	return retval;
}

static NTSTATUS rmi_populate_f01(PSYNA_CONTEXT pDevice)
{
	uint8_t basic_queries[RMI_DEVICE_F01_BASIC_QUERY_LEN];
	uint8_t info[3];
	NTSTATUS status;
	bool has_query42;
	bool has_lts;
	bool has_sensor_id;
	bool has_ds4_queries = false;
	bool has_build_id_query = false;
	bool has_package_id_query = false;
	uint16_t query_offset = pDevice->f01.query_base_addr;
	uint16_t prod_info_addr;
	uint8_t ds4_query_len;

	status = rmi_read_block(pDevice, query_offset, basic_queries,
		RMI_DEVICE_F01_BASIC_QUERY_LEN);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Can not read basic queries from Function 0x1.\n");
		return status;
	}

	has_lts = !!(basic_queries[0] & BIT(2));
	has_sensor_id = !!(basic_queries[1] & BIT(3));
	has_query42 = !!(basic_queries[1] & BIT(7));

	query_offset += 11;
	prod_info_addr = query_offset + 6;
	query_offset += 10;

	if (has_lts)
		query_offset += 20;

	if (has_sensor_id)
		query_offset++;

	if (has_query42) {
		status = rmi_read(pDevice, query_offset, info);
		if (!NT_SUCCESS(status)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Can not read query42.\n");
			return status;
		}
		has_ds4_queries = !!(info[0] & BIT(0));
		query_offset++;
	}

	if (has_ds4_queries) {
		status = rmi_read(pDevice, query_offset, &ds4_query_len);
		if (!NT_SUCCESS(status)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Can not read DS4 Query length.\n");
			return status;
		}
		query_offset++;

		if (ds4_query_len > 0) {
			status = rmi_read(pDevice, query_offset, info);
			if (!NT_SUCCESS(status)) {
				SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Can not read DS4 query.\n");
				return status;
			}

			has_package_id_query = !!(info[0] & BIT(0));
			has_build_id_query = !!(info[0] & BIT(1));
		}
	}

	if (has_package_id_query)
		prod_info_addr++;

	if (has_build_id_query) {
		status = rmi_read_block(pDevice, prod_info_addr, info, 3);
		if (!NT_SUCCESS(status)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Can not read product info.\n");
			return status;
		}

		pDevice->firmware_id = info[1] << 8 | info[0];
		pDevice->firmware_id += info[2] * 65536;
	}

	status = rmi_read_block(pDevice, pDevice->f01.control_base_addr, info,
		2);

	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not read f01 ctrl registers\n");
		return status;
	}

	pDevice->f01_ctrl0 = info[0];

	if (!info[1]) {
		/*
		* Do to a firmware bug in some touchpads the F01 interrupt
		* enable control register will be cleared on reset.
		* This will stop the touchpad from reporting data, so
		* if F01 CTRL1 is 0 then we need to explicitly enable
		* interrupts for the functions we want data for.
		*/
		pDevice->restore_interrupt_mask = true;

		status = rmi_write(pDevice, pDevice->f01.control_base_addr + 1,
			&pDevice->interrupt_enable_mask);
		if (!NT_SUCCESS(status)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not write to control reg 1: %d.\n", status);
			return status;
		}
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Firmware bug fix needed!!! :/\n");
	}

	return status;
}

static NTSTATUS rmi_populate_f11(PSYNA_CONTEXT pDevice)
{
	uint8_t buf[20];
	NTSTATUS status;
	bool has_query9;
	bool has_query10 = false;
	bool has_query11;
	bool has_query12;
	bool has_query27;
	bool has_query28;
	bool has_query36 = false;
	bool has_physical_props;
	bool has_gestures;
	bool has_rel;
	bool has_data40 = false;
	bool has_dribble = false;
	bool has_palm_detect = false;
	unsigned x_size, y_size;
	uint16_t query_offset;

	if (!pDevice->f11.query_base_addr) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "No 2D sensor found, giving up.\n");
		return STATUS_NO_SUCH_DEVICE;
	}

	/* query 0 contains some useful information */
	status = rmi_read(pDevice, pDevice->f11.query_base_addr, buf);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not get query 0: %d.\n", status);
		return status;
	}
	has_query9 = !!(buf[0] & BIT(3));
	has_query11 = !!(buf[0] & BIT(4));
	has_query12 = !!(buf[0] & BIT(5));
	has_query27 = !!(buf[0] & BIT(6));
	has_query28 = !!(buf[0] & BIT(7));

	/* query 1 to get the max number of fingers */
	status = rmi_read(pDevice, pDevice->f11.query_base_addr + 1, buf);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not get NumberOfFingers: %d.\n", status);
		return status;
	}
	pDevice->max_fingers = (buf[0] & 0x07) + 1;
	if (pDevice->max_fingers > 5)
		pDevice->max_fingers = 10;

	pDevice->f11.report_size = pDevice->max_fingers * 5 +
		DIV_ROUND_UP(pDevice->max_fingers, 4);

	if (!(buf[0] & BIT(4))) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "No absolute events, giving up.\n");
		return STATUS_INVALID_DEVICE_REQUEST;
	}

	has_rel = !!(buf[0] & BIT(3));
	has_gestures = !!(buf[0] & BIT(5));

	status = rmi_read(pDevice, pDevice->f11.query_base_addr + 5, buf);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not get absolute data sources: %d.\n", status);
		return status;
	}

	has_dribble = !!(buf[0] & BIT(4));

	/*
	* At least 4 queries are guaranteed to be present in F11
	* +1 for query 5 which is present since absolute events are
	* reported and +1 for query 12.
	*/
	query_offset = 6;

	if (has_rel)
		++query_offset; /* query 6 is present */

	if (has_gestures) {
		/* query 8 to find out if query 10 exists */
		status = rmi_read(pDevice, pDevice->f11.query_base_addr + query_offset + 1, buf);
		if (!NT_SUCCESS(status)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not read gesture information: %d.\n",
				status);
			return status;
		}
		has_palm_detect = !!(buf[0] & BIT(0));
		has_query10 = !!(buf[0] & BIT(2));

		query_offset += 2; /* query 7 and 8 are present */
	}

	if (has_query9)
		++query_offset;

	if (has_query10)
		++query_offset;

	if (has_query11)
		++query_offset;

	/* query 12 to know if the physical properties are reported */
	if (has_query12) {
		status = rmi_read(pDevice, pDevice->f11.query_base_addr
			+ query_offset, buf);
		if (!NT_SUCCESS(status)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not get query 12: %d.\n", status);
			return status;
		}
		has_physical_props = !!(buf[0] & BIT(5));

		if (has_physical_props) {
			query_offset += 1;
			status = rmi_read_block(pDevice, pDevice->f11.query_base_addr
				+ query_offset, buf, 4);
			if (status) {
				SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not read query 15-18: %d.\n",
					status);
				return status;
			}

			x_size = buf[0] | (buf[1] << 8);
			y_size = buf[2] | (buf[3] << 8);

			pDevice->x_size_mm = x_size / 10;
			pDevice->y_size_mm = y_size / 10;

			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "size in mm: %d x %d\n",
				pDevice->x_size_mm, pDevice->y_size_mm);

			/*
			* query 15 - 18 contain the size of the sensor
			* and query 19 - 26 contain bezel dimensions
			*/
			query_offset += 12;
		}
	}

	if (has_query27)
		++query_offset;

	if (has_query28) {
		status = rmi_read(pDevice, pDevice->f11.query_base_addr
			+ query_offset, buf);
		if (!NT_SUCCESS(status)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not get query 28: %d.\n", status);
			return status;
		}

		has_query36 = !!(buf[0] & BIT(6));
	}

	if (has_query36) {
		query_offset += 2;
		status = rmi_read(pDevice, pDevice->f11.query_base_addr
			+ query_offset, buf);
		if (!NT_SUCCESS(status)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not get query 36: %d.\n", status);
			return status;
		}

		has_data40 = !!(buf[0] & BIT(5));
	}


	if (has_data40)
		pDevice->f11.report_size += pDevice->max_fingers * 2;

	status = rmi_read_block(pDevice, pDevice->f11.control_base_addr,
		pDevice->f11_ctrl_regs, RMI_F11_CTRL_REG_COUNT);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not read ctrl block of size 11: %d.\n", status);
		return status;
	}

	/* data->f11_ctrl_regs now contains valid register data */
	pDevice->read_f11_ctrl_regs = true;

	pDevice->max_x = pDevice->f11_ctrl_regs[6] | (pDevice->f11_ctrl_regs[7] << 8);
	pDevice->max_y = pDevice->f11_ctrl_regs[8] | (pDevice->f11_ctrl_regs[9] << 8);

	SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Trackpad Resolution: %d x %d\n", pDevice->max_x, pDevice->max_y);

	if (has_dribble) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Has Dribble\n");
		pDevice->f11_ctrl_regs[0] = pDevice->f11_ctrl_regs[0] & ~BIT(6);
		status = rmi_write(pDevice, pDevice->f11.control_base_addr,
			pDevice->f11_ctrl_regs);
		if (!NT_SUCCESS(status)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not write to control reg 0: %d.\n",
				status);
			return status;
		}
	}

	if (has_palm_detect) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Has Palm Detect\n");
		pDevice->f11_ctrl_regs[11] = pDevice->f11_ctrl_regs[11] & ~BIT(0);
		status = rmi_write(pDevice, pDevice->f11.control_base_addr + 11,
			&pDevice->f11_ctrl_regs[11]);
		if (!NT_SUCCESS(status)) {
			SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not write to control reg 11: %d.\n",
				status);
			return status;
		}
	}

	return status;
}

static NTSTATUS rmi_populate_f30(PSYNA_CONTEXT pDevice)
{
	uint8_t buf[20];
	NTSTATUS status;
	bool has_gpio, has_led;
	unsigned bytes_per_ctrl;
	uint8_t ctrl2_addr;
	int ctrl2_3_length;
	int i;

	/* function F30 is for physical buttons */
	if (!pDevice->f30.query_base_addr) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "No GPIO/LEDs found, giving up.\n");
		return STATUS_NO_SUCH_DEVICE;
	}

	status = rmi_read_block(pDevice, pDevice->f30.query_base_addr, buf, 2);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not get F30 query registers: %d.\n", status);
		return status;
	}

	has_gpio = !!(buf[0] & BIT(3));
	has_led = !!(buf[0] & BIT(2));
	pDevice->gpio_led_count = buf[1] & 0x1f;

	/* retrieve ctrl 2 & 3 registers */
	bytes_per_ctrl = (pDevice->gpio_led_count + 7) / 8;
	/* Ctrl0 is present only if both has_gpio and has_led are set*/
	ctrl2_addr = (has_gpio && has_led) ? bytes_per_ctrl : 0;
	/* Ctrl1 is always be present */
	ctrl2_addr += bytes_per_ctrl;
	ctrl2_3_length = 2 * bytes_per_ctrl;

	pDevice->f30.report_size = bytes_per_ctrl;

	status = rmi_read_block(pDevice, pDevice->f30.control_base_addr + ctrl2_addr,
		buf, ctrl2_3_length);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "can not read ctrl 2&3 block of size %d: %d.\n",
			ctrl2_3_length, status);
		return status;
	}

	pDevice->button_count = 0;
	pDevice->button_mask = 0;
	pDevice->button_state_mask = 0;

	for (i = 0; i < pDevice->gpio_led_count; i++) {
		int byte_position = i >> 3;
		int bit_position = i & 0x07;
		uint8_t dir_byte = buf[byte_position];
		uint8_t data_byte = buf[byte_position + bytes_per_ctrl];
		bool dir = (dir_byte >> bit_position) & BIT(0);
		bool dat = (data_byte >> bit_position) & BIT(0);

		if (dir == 0) {
			/* input mode */
			if (dat) {
				/* actual buttons have pull up resistor */
				pDevice->button_count++;
				SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Button found at Bit 0x%d\n");
				pDevice->button_mask += BIT(i);
				pDevice->button_state_mask += BIT(i);
			}
		}

	}

	return status;
}

NTSTATUS rmi_populate(PSYNA_CONTEXT pDevice) {
	NTSTATUS status;

	status = rmi_set_mode(pDevice, 0);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "PDT set mode failed with code %d\n", status);
		return status;
	}

	status = rmi_scan_pdt(pDevice);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "PDT scan failed with code %d\n", status);
		return status;
	}

	status = rmi_populate_f01(pDevice);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Error while initializing F01 (%d)\n", status);
		return status;
	}

	status = rmi_populate_f11(pDevice);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Error while initializing F11 (%d)\n", status);
		return status;
	}

	status = rmi_populate_f30(pDevice);
	if (!NT_SUCCESS(status)) {
		SynaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Error while initializing F30 (%d)\n", status);
		return status;
	}
	return status;
}

NTSTATUS rmi_set_sleep_mode(PSYNA_CONTEXT pDevice, int sleep) {
	uint8_t f01ctrl0 = (pDevice->f01_ctrl0 & ~0x3) | sleep;

	return rmi_write(pDevice, pDevice->f01.control_base_addr, &f01ctrl0);
}
