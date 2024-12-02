/*
 * Copyright (c) 2022 Balázs Triszka <balika011@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "bsp/board.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/util/queue.h"
#include "pico/multicore.h"

#include "tusb.h"
#include "xbox.h"
#include "isd1200.h"
#include "pins.h"

#include "post.pio.h"

#define QUEUE_CMD_READ_NAND 0
#define QUEUE_CMD_WRITE_NAND 1
#define QUEUE_CMD_READ_EMMC 2
#define QUEUE_CMD_WRITE_EMMC 3
#define QUEUE_CMD_GET_CONFIG 4
#define QUEUE_CMD_READ_CID 5
#define QUEUE_CMD_READ_CSD 6
#define QUEUE_CMD_READ_EXT_CSD 7
#define QUEUE_CMD_INIT_EMMC 8
#define QUEUE_CMD_START_SMC 9
#define QUEUE_CMD_STOP_SMC 10

void core1_stop_smc(void);
void core1_start_smc(void);
uint32_t core1_get_config(void);

// Invoked when device is mounted
void tud_mount_cb(void)
{
	//core1_stop_smc();
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
	core1_start_smc();
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
	(void)remote_wakeup_en;

	core1_start_smc();
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
	//core1_stop_smc();
}

typedef struct
{
	uint32_t cmd;
	uint32_t status;
    uint32_t offset;
	uint8_t data[0x210];
} queue_entry_t;

queue_t xbox_queue;
queue_t usb_queue;

#define GET_VERSION 0x00
#define GET_FLASH_CONFIG 0x01
#define READ_FLASH 0x02
#define WRITE_FLASH 0x03
#define READ_FLASH_STREAM 0x04

#define GET_POST 0x80

#define EMMC_DETECT 0x50
#define EMMC_INIT 0x51
#define EMMC_GET_CID 0x52
#define EMMC_GET_CSD 0x53
#define EMMC_GET_EXT_CSD 0x54
#define EMMC_READ 0x55
#define EMMC_READ_STREAM 0x56
#define EMMC_WRITE 0x57

#define START_SMC 0xC0
#define STOP_SMC 0xC1

#define ISD1200_INIT 0xA0
#define ISD1200_DEINIT 0xA1
#define ISD1200_READ_ID 0xA2
#define ISD1200_READ_FLASH 0xA3
#define ISD1200_ERASE_FLASH 0xA4
#define ISD1200_WRITE_FLASH 0xA5
#define ISD1200_PLAY_VOICE 0xA6
#define ISD1200_EXEC_MACRO 0xA7
#define ISD1200_RESET 0xA8

#define REBOOT_TO_BOOTLOADER 0xFE

#pragma pack(push, 1)
struct cmd
{
	uint8_t cmd;
	uint32_t lba;
};
#pragma pack(pop)

bool stream_emmc = false;
bool do_stream = false;
uint32_t stream_offset_rcvd = 0;
uint32_t stream_offset_sent = 0;
uint32_t stream_end = 0;
void stream()
{
	if (do_stream)
	{
		if (stream_offset_rcvd >= stream_end)
		{
			do_stream = false;
			return;
		}

		if (tud_cdc_write_available() < 4 + (stream_emmc ? 0x200 : 0x210))
			return;

		if (do_stream && !queue_is_full(&xbox_queue) && stream_offset_sent < stream_end)
		{
			queue_entry_t entry;
			entry.offset = stream_offset_sent++;
			entry.cmd = stream_emmc ? QUEUE_CMD_READ_EMMC : QUEUE_CMD_READ_NAND;
			queue_add_blocking(&xbox_queue, &entry);
		}
		if (do_stream && !queue_is_empty(&usb_queue))
		{
			queue_entry_t entry;
			queue_remove_blocking(&usb_queue, &entry);
			++stream_offset_rcvd;
			tud_cdc_write(&entry.status, 4);
			if (entry.status == 0)
			{
				tud_cdc_write(entry.data, stream_emmc ? 0x200 : 0x210);
			} else
			{
				do_stream = false;
				while (stream_offset_rcvd < stream_offset_sent)
				{
					queue_remove_blocking(&usb_queue, &entry);
				}
			}
		}
	}
}

unsigned char reverse(unsigned char b) 
{
   b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
   b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
   b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
   return b;
}

static uint8_t post_put;
static uint8_t post_get;
static uint8_t post_buf[0x100] = {0};

void post_buffer()
{
	for (int i = 0; i <= 8; i++)
	{
		if (pio_sm_is_rx_fifo_empty(pio0, 0))
			continue;
		uint32_t data = pio_sm_get(pio0, 0);
		uint8_t post = reverse(data & 0xFF);
		post_buf[post_put++] = post;
		if (post_get == post_put)
			post_get++;
	}
}

// Invoked when CDC interface received data from host
void tud_cdc_rx_cb(uint8_t itf)
{
	(void)itf;

	uint32_t avilable_data = tud_cdc_available();

	uint32_t needed_data = sizeof(struct cmd);
	{
		uint8_t cmd;
		tud_cdc_peek(&cmd);
		if (cmd == WRITE_FLASH)
			needed_data += 0x210;
		if (cmd == ISD1200_WRITE_FLASH)
			needed_data += 16;
	}

	if (avilable_data >= needed_data)
	{
		struct cmd cmd;

		uint32_t count = tud_cdc_read(&cmd, sizeof(cmd));
		if (count != sizeof(cmd))
			return;

		if (cmd.cmd == GET_VERSION)
		{
			uint32_t ver = 3;
			tud_cdc_write(&ver, 4);
		}
		else if (cmd.cmd == START_SMC)
		{
			core1_start_smc();
		}
		else if (cmd.cmd == STOP_SMC)
		{
			core1_stop_smc();
		}
		else if (cmd.cmd == GET_FLASH_CONFIG)
		{
			uint32_t fc = core1_get_config();
			tud_cdc_write(&fc, 4);
		}
		else if (cmd.cmd == READ_FLASH)
		{
			queue_entry_t entry;
			entry.offset = cmd.lba;
			entry.cmd = QUEUE_CMD_READ_NAND;
			queue_add_blocking(&xbox_queue, &entry);
			queue_remove_blocking(&usb_queue, &entry);
			tud_cdc_write(&entry.status, 4);
			if (entry.status == 0)
				tud_cdc_write(entry.data, 0x210);
		}
		else if (cmd.cmd == WRITE_FLASH)
		{
			queue_entry_t entry;
			entry.offset = cmd.lba;
			tud_cdc_read(entry.data, 0x210);
			entry.cmd = QUEUE_CMD_WRITE_NAND;
			queue_add_blocking(&xbox_queue, &entry);
			uint32_t ret = 0;	// TODO: add errors processing
			tud_cdc_write(&ret, 4);
		}
		else if (cmd.cmd == READ_FLASH_STREAM)
		{
			stream_emmc = false;
			do_stream = true;
			stream_offset_sent = 0;
			stream_offset_rcvd = 0;
			stream_end = cmd.lba;
		}
		else if (cmd.cmd == GET_POST)
		{
			uint8_t len = post_put - post_get;
			tud_cdc_write(&len, 1);
			if (len != 0)
			{
				if (post_get < post_put)
				{
					tud_cdc_write(post_buf + post_get, post_put - post_get);
				} else
				{
					tud_cdc_write(post_buf + post_get, sizeof(post_buf) - post_get);
					tud_cdc_write(post_buf, post_put);
				}
				post_get = post_put = 0;
			}
		}
		if (cmd.cmd == ISD1200_INIT)
		{
			uint8_t ret = isd1200_init() ? 0 : 1;
			tud_cdc_write(&ret, 1);
		}
		if (cmd.cmd == ISD1200_DEINIT)
		{
			isd1200_deinit();
			uint8_t ret = 0;
			tud_cdc_write(&ret, 1);
		}
		else if (cmd.cmd == ISD1200_READ_ID)
		{
			uint8_t dev_id = isd1200_read_id();
			tud_cdc_write(&dev_id, 1);
		}
		else if (cmd.cmd == ISD1200_READ_FLASH)
		{
			uint8_t buffer[512];
			isd1200_flash_read(cmd.lba, buffer);
			tud_cdc_write(buffer, sizeof(buffer));
		}
		if (cmd.cmd == ISD1200_ERASE_FLASH)
		{
			isd1200_chip_erase();
			uint8_t ret = 0;
			tud_cdc_write(&ret, 1);
		}
		else if (cmd.cmd == ISD1200_WRITE_FLASH)
		{
			uint8_t buffer[16];
			uint32_t count = tud_cdc_read(&buffer, sizeof(buffer));
			if (count != sizeof(buffer))
				return;
			isd1200_flash_write(cmd.lba, buffer);
			uint32_t ret = 0;
			tud_cdc_write(&ret, 4);
		}
		else if (cmd.cmd == ISD1200_PLAY_VOICE)
		{
			isd1200_play_vp(cmd.lba);
			uint8_t ret = 0;
			tud_cdc_write(&ret, 1);
		}
		else if (cmd.cmd == ISD1200_EXEC_MACRO)
		{
			isd1200_exe_vm(cmd.lba);
			uint8_t ret = 0;
			tud_cdc_write(&ret, 1);
		}
		if (cmd.cmd == ISD1200_RESET)
		{
			isd1200_reset();
			uint8_t ret = 0;
			tud_cdc_write(&ret, 1);
		}
		else if (cmd.cmd == REBOOT_TO_BOOTLOADER)
		{
			reset_usb_boot(0, 0);
		}
		else if (cmd.cmd == EMMC_DETECT)
		{
			uint32_t fc = core1_get_config();
			int emmc_detect_result = (fc & 0xF0000000) == 0xC0000000;
			tud_cdc_write(&emmc_detect_result, 1);
		}
		else if (cmd.cmd == EMMC_INIT)
		{
			queue_entry_t entry;
			entry.cmd = QUEUE_CMD_INIT_EMMC;
			queue_add_blocking(&xbox_queue, &entry);
			queue_remove_blocking(&usb_queue, &entry);
			tud_cdc_write(&entry.status, 4);
		}
		else if (cmd.cmd == EMMC_GET_CID)
		{
			queue_entry_t entry;
			entry.cmd = QUEUE_CMD_READ_CID;
			queue_add_blocking(&xbox_queue, &entry);
			queue_remove_blocking(&usb_queue, &entry);
			tud_cdc_write(entry.data, 16);
		}
		else if (cmd.cmd == EMMC_GET_CSD)
		{
			queue_entry_t entry;
			entry.cmd = QUEUE_CMD_READ_CSD;
			queue_add_blocking(&xbox_queue, &entry);
			queue_remove_blocking(&usb_queue, &entry);
			tud_cdc_write(entry.data, 16);
		}
		else if (cmd.cmd == EMMC_GET_EXT_CSD)
		{
			queue_entry_t entry;
			entry.cmd = QUEUE_CMD_READ_EXT_CSD;
			queue_add_blocking(&xbox_queue, &entry);
			queue_remove_blocking(&usb_queue, &entry);
			tud_cdc_write(entry.data, 0x200);
		}
		else if (cmd.cmd == EMMC_READ)
		{
			queue_entry_t entry;
			entry.offset = cmd.lba;
			entry.cmd = QUEUE_CMD_READ_EMMC;
			queue_add_blocking(&xbox_queue, &entry);
			queue_remove_blocking(&usb_queue, &entry);
			tud_cdc_write(&entry.status, 4);
			if (entry.status == 0)
				tud_cdc_write(entry.data, 0x200);
		}
		else if (cmd.cmd == EMMC_READ_STREAM)
		{
			stream_emmc = true;
			do_stream = true;
			stream_offset_sent = 0;
			stream_offset_rcvd = 0;
			stream_end = cmd.lba;
		}
		else if (cmd.cmd == EMMC_WRITE)
		{
			queue_entry_t entry;
			entry.offset = cmd.lba;
			tud_cdc_read(entry.data, 0x200);
			entry.cmd = QUEUE_CMD_WRITE_EMMC;
			queue_add_blocking(&xbox_queue, &entry);
			uint32_t ret = 0;	// TODO: add errors processing
			tud_cdc_write(&ret, 4);
		}

		tud_cdc_write_flush();
	}
}

void tud_cdc_tx_complete_cb(uint8_t itf)
{
	(void)itf;
}

void core1_stop_smc()
{
	queue_entry_t entry;
	entry.cmd = QUEUE_CMD_STOP_SMC;
	queue_add_blocking(&xbox_queue, &entry);
}

void core1_start_smc()
{
	queue_entry_t entry;
	entry.cmd = QUEUE_CMD_START_SMC;
	queue_add_blocking(&xbox_queue, &entry);
}

uint32_t core1_get_config()
{
	queue_entry_t entry;
	entry.cmd = QUEUE_CMD_GET_CONFIG;
	queue_add_blocking(&xbox_queue, &entry);
	queue_remove_blocking(&usb_queue, &entry);
	return entry.status;
}

void main_core1(void)
{
	while(1)
	{
		queue_entry_t entry;
		queue_peek_blocking(&xbox_queue, &entry);
		if (entry.cmd == QUEUE_CMD_READ_NAND)
		{
			entry.status = xbox_nand_read_block(entry.offset, entry.data, entry.data + 0x200);
			queue_add_blocking(&usb_queue, &entry);
		} else if (entry.cmd == QUEUE_CMD_READ_EMMC)
		{
			entry.status = xbox_emmc_read_block(entry.offset, entry.data);
			queue_add_blocking(&usb_queue, &entry);
		} else if (entry.cmd == QUEUE_CMD_WRITE_NAND)
		{
			entry.status = xbox_nand_write_block(entry.offset, entry.data, entry.data + 0x200);
			//queue_add_blocking(&usb_queue, &entry); 	// TODO: add errors processing
		} else if (entry.cmd == QUEUE_CMD_WRITE_EMMC)
		{
			entry.status = xbox_emmc_write_block(entry.offset, entry.data);
			//queue_add_blocking(&usb_queue, &entry); 	// TODO: add errors processing
		} else if (entry.cmd == QUEUE_CMD_INIT_EMMC)
		{
			entry.status = xbox_emmc_init(entry.offset, entry.data);
			queue_add_blocking(&usb_queue, &entry);
		} else if (entry.cmd == QUEUE_CMD_READ_CID)
		{
			entry.status = xbox_emmc_read_cid(entry.data);
			queue_add_blocking(&usb_queue, &entry);
		} else if (entry.cmd == QUEUE_CMD_READ_CSD)
		{
			entry.status = xbox_emmc_read_csd(entry.data);
			queue_add_blocking(&usb_queue, &entry);
		} else if (entry.cmd == QUEUE_CMD_READ_EXT_CSD)
		{
			entry.status = xbox_emmc_read_ext_csd(entry.data);
			queue_add_blocking(&usb_queue, &entry);
		} else if (entry.cmd == QUEUE_CMD_GET_CONFIG)
		{
			entry.status = xbox_get_flash_config();
			queue_add_blocking(&usb_queue, &entry);
		} else if (entry.cmd == QUEUE_CMD_START_SMC)
		{
			xbox_start_smc();
			//queue_add_blocking(&usb_queue, &entry);
		} else if (entry.cmd == QUEUE_CMD_STOP_SMC)
		{
			xbox_stop_smc();
			//queue_add_blocking(&usb_queue, &entry);
		}
		queue_remove_blocking(&xbox_queue, &entry);
	}
}

int post_init()
{
	int offset = pio_add_program(pio0, &poster_program);

    pio_sm_config c = poster_program_get_default_config(offset);
    sm_config_set_in_pins(&c, SMC_POST_0);
	sm_config_set_jmp_pin(&c, SMC_CPU_RST);
	for(int i = SMC_POST_0; i < SMC_POST_0 + 8; i++)
	{
		gpio_pull_up(i);
	}
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio0, 0, offset, &c);
    pio_sm_set_enabled(pio0, 0, true);
}

int main(void)
{
	uint32_t freq = clock_get_hz(clk_sys);
	clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, freq, freq);

	xbox_init();
	tusb_init();
	post_init();

	queue_init(&xbox_queue, sizeof(queue_entry_t), 8);
    queue_init(&usb_queue, sizeof(queue_entry_t), 8);

	multicore_launch_core1(main_core1);

	while (1)
	{
		post_buffer();
		tud_task();
		stream();
	}

	return 0;
}