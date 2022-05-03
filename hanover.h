#include <stdint.h>

#define HANOVER_MQ_NAME    "/hanovermq"
#define HANOVER_MQ_MAXMSG  10
#define HANOVER_MQ_MSGSIZE 4096

struct hanover_mqmsg {
	uint16_t len;
	char msg[2048];
};
