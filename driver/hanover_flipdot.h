#include <stdint.h>
#include <errno.h>

struct hanover_display {
	uint8_t addr;
	uint16_t n_rows;
	uint16_t n_cols;
	int fd;
};

int32_t hanover_init(struct hanover_display **display, char *ttydev, uint8_t addr,
	uint16_t n_rows, uint16_t cols);
int32_t hanover_free(struct hanover_display *display);
int32_t hanover_write(struct hanover_display *display, uint8_t *matrix);
