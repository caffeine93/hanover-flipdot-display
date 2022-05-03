#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>
#include <mqueue.h>

#include <driver/hanover_flipdot.h>
#include <font8x8_basic.h>

#include <hanover.h>

#define DISPLAY_RS485_PORT "/dev/ttyAMA0"
#define DISPLAY_N_ROWS 16
#define DISPLAY_N_COLS 96

#define INFO(fmt, args...) syslog(LOG_INFO, fmt, ##args);
#define ERR(fmt, args...) syslog(LOG_ERR, fmt, ##args);

static inline void set_dot(uint8_t *display_area, uint16_t row, uint16_t col, uint8_t dot)
{
	if (dot)
		display_area[(row/8) * DISPLAY_N_COLS + col] |= (1 << (row % 8));
	else
		display_area[(row/8) * DISPLAY_N_COLS + col] &= ~(1 << (row % 8));
}

static inline void get_font_char(char c, uint8_t *font_char)
{
	memcpy(font_char, font8x8_basic[c], 8);
}

static uint16_t sprintf_display(char *msg, uint8_t *display_area, uint16_t rows, uint16_t cols)
{
	uint8_t font_char[8] = {0};
	uint8_t font_char_row = 0x00;
	uint16_t bit_row = 0;
	uint16_t bit_col = 0;
	uint16_t char_row = 0;
	uint16_t char_col = 0;
	char *word = NULL;
	uint16_t processed_chars = 0;

	while (*msg) {
		/* skip whitespaces at the beginning of the row */
		if ((*msg == ' ') && (char_col == 0))
			goto next;

		get_font_char(*msg, font_char);
		bit_row = char_row * 8;
		bit_col = char_col * 8;

		for (uint8_t i = 0; i < 8; i++) {
			font_char_row = font_char[i];
			for (uint8_t j = 0; j < 8; j++)
				set_dot(display_area, bit_row+i, bit_col+j, font_char_row & (1 << j));
		}

		/* we're at the beginning of a new word now */
		if ((*msg == ' ') && *(msg + 1) && *(msg + 1) != ' ') {
			word = msg + 1;
			while (*word && (*word != ' '))
				 word++;

			/* if the word can't fit in a single row anyway,
			 * no point in optimizing */
			if ((word - msg - 1) > (cols/8))
				goto next;
			/* if the word can fit in a single row, but not
			 * from the current position in the row, jump
			 * to the beginning of a new row if possible,
			 * or otherwise, return now so that new call
			 * will place it at the beginning of the first row */
			if ((word - msg - 1) > (cols/8 - char_col - 1 - 1)) {
				if (char_row < rows/8 - 1) {
					char_row++;
					char_col = 0;
					goto next;
				}
				else
					return ++processed_chars;
			}
		}


		if (char_col < cols/8 - 1)
			char_col++;
		else {
			char_row++;
			char_col = 0;
		}

next:
		msg++;
		processed_chars++;
	}

	return processed_chars;
}

int main(void)
{
	uint8_t display_area[DISPLAY_N_ROWS/8][DISPLAY_N_COLS] = {0};
	uint8_t *display_area_ptr = (uint8_t *)display_area;
	int ret = 0;
	struct hanover_display *display = NULL;
	struct mq_attr hanover_mq_attr = {0};
	mqd_t hanover_mq;
	uint8_t mq_buff[HANOVER_MQ_MSGSIZE * 5];
	struct timespec mq_timeout = {0};
	struct hanover_mqmsg *msg = NULL;
	char msg_display[(DISPLAY_N_COLS/8) * (DISPLAY_N_ROWS/8) + 1] = {'\0'};
	uint16_t n_printed_chars = 0;
	char *tmp_msg = NULL;

	ret = hanover_init(&display, DISPLAY_RS485_PORT, 0x01,
		DISPLAY_N_ROWS, DISPLAY_N_COLS);
	if (ret) {
		ERR("Failed initializing the Hanover display: %d\n", -ret);
		return ret;
	}

	sprintf_display("Initializing display...", display_area_ptr, DISPLAY_N_ROWS, DISPLAY_N_COLS);
	ret = hanover_write(display, display_area_ptr);
	if (ret)
		ERR("Failed writing to display: %d\n", -ret);

	hanover_mq_attr.mq_maxmsg = HANOVER_MQ_MAXMSG;
	hanover_mq_attr.mq_msgsize = HANOVER_MQ_MSGSIZE;
	hanover_mq = mq_open(HANOVER_MQ_NAME, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR,
		&hanover_mq_attr);
	if (hanover_mq < 0) {
		ERR("Failed to create mq @ %s: %d\n", HANOVER_MQ_NAME, -errno);
		goto exit;
	}

	mq_timeout.tv_sec = 5;
	mq_timeout.tv_nsec = 0;

	do {
		ret = mq_timedreceive(hanover_mq, mq_buff, sizeof(mq_buff), NULL, &mq_timeout);
		if (ret > 0) {
			msg = (struct hanover_mqmsg *)mq_buff;
			INFO("Received mq msg, len = %u\n", msg->len);
			tmp_msg = msg->msg;

			while (msg->len) {
				memset(display_area, 0, sizeof(display_area));
				snprintf(msg_display, sizeof(msg_display), tmp_msg);
				msg_display[sizeof(msg_display) - 1] = '\0';
				INFO("Printing %s to display...\n", msg_display);
				n_printed_chars = sprintf_display(msg_display, display_area_ptr, DISPLAY_N_ROWS, DISPLAY_N_COLS);
				if (hanover_write(display, display_area_ptr))
					ERR("Failed writing to display\n");
				tmp_msg += n_printed_chars;
				if (msg->len >= n_printed_chars)
					msg->len -= n_printed_chars;
				else
					msg->len = 0;

				usleep(2000000);
			}
		}
	} while (ret > 0 || ((ret == -1) && errno == ETIMEDOUT));

	if (ret < 0)
		INFO("MQ receive failed: %d", -errno);

exit:
	if (hanover_mq >= 0) {
		ret = mq_close(hanover_mq);
		if (ret)
			ERR("Failed to close mq: %d\n", -errno);
	}

	ret = mq_unlink(HANOVER_MQ_NAME);
	if (ret)
		ERR("Failed deleting mq @ %s\n", HANOVER_MQ_NAME);

	ret = hanover_free(display);
	if (ret)
		ERR("Failed to release Hanover display resources: %d\n", -ret);
	return ret;
}
