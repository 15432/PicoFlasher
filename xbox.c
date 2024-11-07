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

#include <string.h>
#include "pico/stdlib.h"
#include "pins.h"
#include "spiex.h"
#include "pio_spi.h"

void xbox_init()
{
	gpio_init(SMC_DBG_EN);
	gpio_put(SMC_DBG_EN, 1);
	gpio_set_dir(SMC_DBG_EN, GPIO_OUT);

	gpio_init(SMC_RST_XDK_N);
	gpio_put(SMC_RST_XDK_N, 1);
	gpio_set_dir(SMC_RST_XDK_N, GPIO_OUT);

	gpio_init(SPI_SS_N);
	gpio_put(SPI_SS_N, 1);
	gpio_set_dir(SPI_SS_N, GPIO_OUT);
}

void xbox_start_smc()
{
	spiex_deinit();

	gpio_put(SMC_DBG_EN, 0);
	gpio_put(SMC_RST_XDK_N, 0);

	sleep_ms(50);

	gpio_put(SMC_RST_XDK_N, 1);
}

void xbox_stop_smc()
{
	gpio_put(SMC_DBG_EN, 0);

	sleep_ms(50);

	gpio_put(SPI_SS_N, 0);
	gpio_put(SMC_RST_XDK_N, 0);

	sleep_ms(50);

	gpio_put(SMC_DBG_EN, 1);
	gpio_put(SMC_RST_XDK_N, 1);

	sleep_ms(50);

	gpio_put(SPI_SS_N, 1);

	sleep_ms(50);

	spiex_init();
}

uint32_t xbox_get_flash_config()
{
	static uint32_t flash_config = 0;
	if (!flash_config)
		flash_config = spiex_read_reg(0);

	return flash_config;
}

uint16_t xbox_nand_get_status()
{
	return spiex_read_reg(0x04);
}

void xbox_nand_clear_status()
{
	spiex_write_reg(0x04, spiex_read_reg(0x04));
}

int xbox_nand_wait_ready(uint16_t timeout)
{
	do
	{
		if (!(xbox_nand_get_status() & 0x01))
			return 0;
	} while (timeout--);

	return 1;
}

int xbox_nand_read_block(uint32_t lba, uint8_t *buffer, uint8_t *spare)
{
	xbox_nand_clear_status();

	spiex_write_reg(0x0C, lba << 9);

	spiex_write_reg(0x08, 0x03);

	if (xbox_nand_wait_ready(0x1000))
		return 0x8000 | xbox_nand_get_status();

	spiex_write_reg(0x0C, 0);

	uint8_t *end = buffer + 0x200;
	while (buffer < end)
	{
		spiex_write_reg(0x08, 0x00);

		*(uint32_t *) buffer = spiex_read_reg(0x10);
		buffer += 4;
	}

	end = spare + 0x10;
	while (spare < end)
	{
		spiex_write_reg(0x08, 0x00);

		*(uint32_t *)spare = spiex_read_reg(0x10);
		spare += 4;
	}

	return 0;
}

int xbox_nand_erase_block(uint32_t lba)
{
	xbox_nand_clear_status();

	spiex_write_reg(0x00, spiex_read_reg(0x00) | 0x08);

	spiex_write_reg(0x0C, lba << 9);

	spiex_write_reg(0x08, 0xAA);
	spiex_write_reg(0x08, 0x55);
	spiex_write_reg(0x08, 0x05);

	if (xbox_nand_wait_ready(0x1000))
		return 0x8000 | xbox_nand_get_status();

	return 0;
}

int xbox_nand_write_block(uint32_t lba, uint8_t *buffer, uint8_t *spare)
{
	int flash_config = xbox_get_flash_config();

	int major = (flash_config >> 17) & 3;
	int minor = (flash_config >> 4) & 3;

	int blocksize = 0x4000;
	if (major >= 1)
	{
		if (minor == 2)
			blocksize = 0x20000;
		else if (minor == 3)
			blocksize = 0x40000;
	}

	int sectors_in_block = blocksize  / 0x200;

	// erase ereases `blocksize` bytes
	if (lba % sectors_in_block == 0)
	{
		int ret = xbox_nand_erase_block(lba);
		if (ret)
			return ret;
	}

	xbox_nand_clear_status();

	spiex_write_reg(0x0C, 0);

	uint8_t *end = buffer + 0x200;
	while (buffer < end)
	{
		spiex_write_reg(0x10, *(uint32_t *)buffer);

		spiex_write_reg(0x08, 0x01);

		buffer += 4;
	}

	end = spare + 0x10;
	while (spare < end)
	{
		spiex_write_reg(0x10, *(uint32_t *)spare);

		spiex_write_reg(0x08, 0x01);

		spare += 4;
	}

	if (xbox_nand_wait_ready(0x1000))
		return 0x8000 | xbox_nand_get_status();

	spiex_write_reg(0x0C, lba << 9);

	if (xbox_nand_wait_ready(0x1000))
		return 0x8000 | xbox_nand_get_status();

	spiex_write_reg(0x08, 0x55);
	spiex_write_reg(0x08, 0xAA);
	spiex_write_reg(0x08, 0x04);

	if (xbox_nand_wait_ready(0x1000))
		return 0x8000 | xbox_nand_get_status();

	return 0;
}

#define SD_OK (0)
#define SD_ERR_TIMEOUT (-1)
#define SD_ERR_BAD_RESPONSE (-2)
#define SD_ERR_CRC (-3)
#define SD_ERR_BAD_PARAM (-4)

static uint32_t xbox_emmc_get_ints()
{
	return spiex_read_reg(0x30);
}

static void xbox_emmc_clear_ints(uint32_t value)
{
	return spiex_write_reg(0x30, value);
}

static void xbox_emmc_clear_all_ints()
{
	xbox_emmc_clear_ints(xbox_emmc_get_ints());
}

static int xbox_emmc_wait_ints(uint32_t value, int timeout_ms)
{
	absolute_time_t wait_timeout = make_timeout_time_ms(timeout_ms);
	do
	{
		uint32_t ints = xbox_emmc_get_ints();
		if ((ints & value) == value)
		{
			return SD_OK;
		}
	} while (!time_reached(wait_timeout));
	return SD_ERR_TIMEOUT;
}

void xbox_emmc_execute(uint32_t reg_4, uint32_t reg_8, uint32_t reg_c)
{
	xbox_emmc_clear_all_ints();
	spiex_write_reg(0x04, reg_4);
	spiex_write_reg(0x08, reg_8);
	spiex_write_reg(0x0C, reg_c);
}

int xbox_emmc_init()
{
	spiex_write_reg(0x2C, spiex_read_reg(0x2C) | (1 << 24));
	absolute_time_t init_timeout = make_timeout_time_ms(5000);
	while (!time_reached(init_timeout))
	{
		if (spiex_read_reg(0x3C) & 0x1000000)
			break;
	}
	if (time_reached(init_timeout))
	{
		return SD_ERR_TIMEOUT;
	}
	return SD_OK;
}

static int xbox_emmc_deselect_card()
{
	xbox_emmc_execute(0, 0, 0x7000000);
	return xbox_emmc_wait_ints(1, 100);
}

static int xbox_emmc_read_cid_csd(uint8_t * buf, int is_cid)
{
	xbox_emmc_execute(0, 0xffff0000, is_cid ? 0x9010000 : 0xA010000);
	int ret = xbox_emmc_wait_ints(1, 100);
	if (!ret)
	{
		for (int i = 0x10; i < 0x20; i += 4)
		{
			uint32_t data = spiex_read_reg(i);
			memcpy(buf, &data, 4);
			buf += 4;
		}
	}
    return ret;
}

int xbox_emmc_read_cid(uint8_t * cid)
{
	return xbox_emmc_read_cid_csd(cid, 1);
}

int xbox_emmc_read_csd(uint8_t * csd)
{
	return xbox_emmc_read_cid_csd(csd, 0);
}

static int xbox_emmc_select_card()
{
	xbox_emmc_execute(0, 0xffff0000, 0x71a0000);
	return xbox_emmc_wait_ints(1, 100);
}

static int xbox_emmc_set_blocklen(int blocklen)
{
	xbox_emmc_execute(0x200, blocklen, 0x101a0000);
	return xbox_emmc_wait_ints(1, 100);
}

static int xbox_emmc_read_block_ext_csd(uint8_t * buf, int block, int is_block)
{
	int ret = xbox_emmc_select_card();
	if (ret)
		return ret;
	if (is_block)
	{
		ret = xbox_emmc_set_blocklen(0x200);
		if (ret)
		{
			xbox_emmc_deselect_card();
			return ret;
		}
	}
	xbox_emmc_execute(0x10200, block << 9, is_block ? 0x113a0010 : 0x83A0010);
	ret = xbox_emmc_wait_ints(0x21, 1500);
	if (!ret)
	{
		for (int i = 0; i < 0x200; i += 4)
		{
			uint32_t data = spiex_read_reg(0x20);
			memcpy(buf + i, &data, 4);
		}
	}
	xbox_emmc_deselect_card();
	return ret;
}

int xbox_emmc_read_ext_csd(uint8_t *ext_csd)
{
	return xbox_emmc_read_block_ext_csd(ext_csd, 0, 0);
}

int xbox_emmc_read_block(int lba, uint8_t *buf)
{
	return xbox_emmc_read_block_ext_csd(buf, lba, 1);
}

int xbox_emmc_write_block(int lba, uint8_t *buf)
{
	int ret = xbox_emmc_select_card();
	if (ret)
		return ret;
	ret = xbox_emmc_set_blocklen(0x200);
	if (ret)
		return ret;
	xbox_emmc_execute(0x10200, lba << 9, 0x183a0000);
	ret = xbox_emmc_wait_ints(1, 100);
	if (!ret)
	{
		for (int i = 0; i < 0x200; i += 4)
		{
			uint32_t data;
			memcpy(&data, buf + i, 4);
			spiex_write_reg(0x20, data);
		}
		ret = xbox_emmc_wait_ints(0x12, 1500);
	}
	xbox_emmc_deselect_card();
	return ret;
}
