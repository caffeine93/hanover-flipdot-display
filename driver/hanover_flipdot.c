/*
 * MIT License
 *
 * Copyright (c) 2022 Luka Culic Viskota
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE
 *
 * hanover_flipdot.c - driver functions for Hanover Display operation
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <termios.h>

#include <hanover_flipdot.h>

#define HANOVER_RS485_BAUDRATE B4800

#define HANOVER_FRAME_START 0x02
#define HANOVER_FRAME_END   0x03
#define HANOVER_FRAME_ADDR1 0x31
#define HANOVER_FRAME_ADDR2(x) (0x30 + x)
#define HANOVER_FRAME_RES1(x) (0x30 +  x)
#define HANOVER_FRAME_RES2(x) (0x30 + x)

#define HNIBBLE(x) (((uint8_t)x) >> 4)
#define LNIBBLE(x) (((uint8_t)x) & 0x0f)

#define ASCII_BYTE(x) ((x >= 0x0a) ? ('A' + (x - 0x0a)) : ('0' + x))
#define HANOVER_FRAME_ASCII_HBYTE(x) (ASCII_BYTE(HNIBBLE(x)))
#define HANOVER_FRAME_ASCII_LBYTE(x) (ASCII_BYTE(LNIBBLE(x)))

#define INFO(fmt, args...) syslog(LOG_INFO, fmt, ##args);
#define ERR(fmt, args...) syslog(LOG_ERR, fmt, ##args);

static int32_t configure_tty(int fd)
{
	struct termios tty_settings = {0};
	int ret = 0;

	if (fd < 0)
		return -EINVAL;

	ret = tcgetattr(fd, &tty_settings);
	if (ret)
		return ret;

	ret = cfsetspeed(&tty_settings, HANOVER_RS485_BAUDRATE);
	if (ret)
		return ret;

	/* 8 databits */
	tty_settings.c_cflag &= ~CSIZE;
	tty_settings.c_cflag |= CS8;
	/* 1 stopbit */
	tty_settings.c_cflag &= ~CSTOPB;
	/* no parity */
	tty_settings.c_cflag &= ~PARENB;
	/* raw mode */
	cfmakeraw(&tty_settings);

	ret = tcsetattr(fd, TCSANOW, &tty_settings);

	return ret;
}

int32_t hanover_init(struct hanover_display **display, char *ttydev, uint8_t addr,
		uint16_t n_rows, uint16_t n_cols)
{
	int fd = -1;
	int ret = 0;
	if (!display || !ttydev || !strlen(ttydev))
		return -EINVAL;

	fd = open(ttydev, O_RDWR);
	if (fd < 0)
		return -EFAULT;

	ret = configure_tty(fd);
	if (ret) {
		close(fd);
		return -EFAULT;
	}

        *display = malloc(sizeof(**display));
        if (!(*display)) {
                close(fd);
                return -ENOMEM;
        }

	(*display)->addr = addr;
	(*display)->fd = fd;
	(*display)->n_rows = n_rows;
	(*display)->n_cols = n_cols;

	return 0;
}

int32_t hanover_free(struct hanover_display *display)
{
	if (!display)
		return -EALREADY;

	close(display->fd);
	free(display);

	return 0;
}

/*
 * C is a row-major language, and the Hanover display firmware expects
 * to receive bytes in a column-major order, thus we need to transform
 * this by appending ASCII bytes from left-to-right top-to-bottom
 */
static void transform_data(uint8_t *ascii_bytes, uint8_t *input_bytes,
		uint16_t rows, uint16_t cols)
{
	uint16_t linear_n = 0;
	uint16_t n = 0;

	for (uint16_t col = 0; col < cols; col++) {
		for (uint16_t row = 0; row < rows/8; row++) {
			linear_n = row * cols + col;
			ascii_bytes[n*2] = HANOVER_FRAME_ASCII_HBYTE(input_bytes[linear_n]);
			ascii_bytes[n*2 + 1] = HANOVER_FRAME_ASCII_LBYTE(input_bytes[linear_n]);

			n++;
		}
	}
}

static uint16_t calc_chksum(uint8_t *data, uint16_t n_data)
{
	uint16_t sum = 0;

	for (uint16_t i = 0; i < n_data; i++)
		sum += data[i];

	sum -= HANOVER_FRAME_START;
	sum &= 0xff;
	sum ^= 0xff;
	sum += 1;

	return sum;
}

/*
 * Hanover displays use a proprietary protocol which transmits frames of the
 * following structure and sizes:
 * |-START-|--ADDR--|--RES--|------DATA------|-END-|--CHKSUM--|
 * | 1byte | 2byte  | 2byte |  variable size |1byte|   2byte  |
 */

static uint8_t *matrix_to_raw(uint8_t *matrix, uint16_t rows,
		uint16_t cols, uint8_t addr, uint16_t *n_data)
{
	uint16_t res = (rows * cols) / 8;
	uint16_t chksum = 0;
	uint8_t *data_frame = NULL;
	uint16_t data_frame_len =  1 + 2 + 2 + (res * 2) + 1 + 2;

	data_frame = malloc(data_frame_len);
	if (!data_frame)
		return NULL;

	data_frame[0] = HANOVER_FRAME_START;
	data_frame[1] = HANOVER_FRAME_ADDR1;
	data_frame[2] = HANOVER_FRAME_ADDR2(addr);
	data_frame[3] = HANOVER_FRAME_ASCII_HBYTE(res & 0xff);
	data_frame[4] = HANOVER_FRAME_ASCII_LBYTE(res & 0xff);
	transform_data(data_frame + 5, matrix, rows, cols);
	data_frame[data_frame_len - 1 - 2] = HANOVER_FRAME_END;
	chksum = calc_chksum(data_frame, data_frame_len - 2);
	data_frame[data_frame_len - 2] = HANOVER_FRAME_ASCII_HBYTE(chksum & 0xff);
	data_frame[data_frame_len - 1] = HANOVER_FRAME_ASCII_LBYTE(chksum & 0xff);

	*n_data = data_frame_len;

	return data_frame;
}

static void dbg_dump_hanover_frame(uint8_t *frame, uint16_t size)
{
	char frame_msg[5000] = {'\0'};
	uint16_t len = 0;

	len = snprintf(frame_msg, sizeof(frame_msg), "Dumping Hanover frame: ");
	for (uint16_t i = 0; i < size; i++)
		len += snprintf(frame_msg + len, sizeof(frame_msg) - len, "0x%02x ", frame[i]);

	INFO(frame_msg);
}

static int32_t write_raw_tty(int fd, uint8_t *data, uint16_t data_sz)
{
	int remain_len = data_sz;
	int ret = 0;

	if ((fd < 0) | !data | !data_sz)
		return -EINVAL;

	while ((ret = write(fd, data, remain_len)) > 0) {
		remain_len -= ret;
		data += ret;
	}

	if (remain_len)
		return -EFAULT;
	else
		return 0;
}

int32_t hanover_write(struct hanover_display *display, uint8_t *matrix)
{
	uint8_t *raw_data = NULL;
	uint16_t data_sz = 0;
	int ret = 0;

	if (!display || !matrix)
		return -EINVAL;

	raw_data = matrix_to_raw(matrix, display->n_rows, display->n_cols,
		display->addr, &data_sz);
	if (!raw_data)
		return -EFAULT;

	dbg_dump_hanover_frame(raw_data, data_sz);

	ret = write_raw_tty(display->fd, raw_data, data_sz);
	free(raw_data);

	return ret;
}
