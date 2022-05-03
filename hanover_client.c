#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>
#include <mqueue.h>

#include <libxml/xmlreader.h>
#include <hanover.h>
#include <curl/curl.h>

#define INFO(fmt, args...) syslog(LOG_INFO, fmt, ##args);
#define ERR(fmt, args...) syslog(LOG_ERR, fmt, ##args);

#define RSS_URL "https://www.theguardian.com/world/rss"
#define MAX_RSS_XML_SZ (1024 * 1024)

struct rss_data {
	uint32_t data_sz;
	uint8_t *data;
};

size_t rss_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct rss_data *data = userdata;
	size_t recv_size = size * nmemb;

	if (!data || !data->data)
		return 0;

	if ((data->data_sz + recv_size) > MAX_RSS_XML_SZ) {
		ERR("CURL: not enough space left in allocated buffer\n");
		return 0;
	}

	memcpy(data->data + data->data_sz, ptr, recv_size);
	data->data_sz += recv_size;
	return recv_size;
}

int main(int argc, char **argv)
{
	struct hanover_mqmsg msg = {0};
	mqd_t hanover_mq;
	int ret = 0;
	CURL *curl_handle = NULL;
	CURLcode curl_res = 0;
	struct rss_data data = {0};
	xmlTextReaderPtr xml_reader;
	const xmlChar *xml_node_name, *xml_node_value;
	int xml_ret = 0;
	uint32_t max_news = UINT32_MAX;

	if (argc == 2) {
		ret = sscanf(argv[1], "%u", &max_news);
		if (ret <= 0)
			ERR("Invalid argument provided for max_news\n");
	}

	LIBXML_TEST_VERSION

	curl_res = curl_global_init(CURL_GLOBAL_ALL);
	if (curl_res != CURLE_OK) {
		ERR("CURL global_init() failed\n");
		goto exit;
	}

	curl_handle = curl_easy_init();
	if (!curl_handle) {
		ERR("CURL: easy_init() failed\n");
		goto exit;
	}

	curl_res = curl_easy_setopt(curl_handle, CURLOPT_URL, RSS_URL);
	if (curl_res != CURLE_OK) {
		ERR("CURL: failed setting CURLOPT_URL\n");
		goto exit;
	}

	curl_res = curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, rss_write);
	if (curl_res != CURLE_OK) {
		ERR("CURL: failed setting CURLOPT_WRITEFUNCTION\n");
		goto exit;
	}

	data.data_sz = 0;
	data.data = malloc(MAX_RSS_XML_SZ);
	if (!data.data) {
		ERR("Failed to allocate buffer for RSS XML\n");
		goto exit;
	}

	curl_res = curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&data);
	if (curl_res != CURLE_OK) {
		ERR("CURL: failed to data buffer\n");
		goto exit;
	}

	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

	curl_res = curl_easy_perform(curl_handle);
	if (curl_res != CURLE_OK) {
		ERR("CURL: failed performing, %d\n", curl_res);
		goto exit;
	}

	xml_reader = xmlReaderForMemory(data.data, data.data_sz, NULL, NULL, 0);
	if (!xml_reader) {
		ERR("Failed to create xmlReader\n");
		goto exit;
	}

        hanover_mq = mq_open(HANOVER_MQ_NAME, O_RDWR);
        if (hanover_mq < 0) {
                ERR("Failed to open mq @ %s, %d\n", HANOVER_MQ_NAME, -errno);
                goto exit;
        }

	do {
		xml_ret = xmlTextReaderRead(xml_reader);
		if (xml_ret > 0) {
			xml_node_name = xmlTextReaderConstName(xml_reader);
			if (xml_node_name && !strcasecmp(xml_node_name, "title")) {
				xml_node_value = xmlTextReaderReadInnerXml(xml_reader);
				if (xml_node_value && strlen(xml_node_value)) {
					snprintf(msg.msg, sizeof(msg.msg), "%s", xml_node_value);
					msg.len = strlen(msg.msg);

					INFO("Sending msg to daemon, len = %u, msg = %s\n", msg.len, msg.msg);
					ret = mq_send(hanover_mq, (uint8_t *)&msg, sizeof(msg), 0);
					if (ret)
						ERR("Failed sending mq msg: %d\n", -errno);

					usleep(2000000);
					max_news--;
				}
			}
		}
	} while ((xml_ret > 0) && max_news);

	if (ret < 0) {
		ERR("Failed parsing the RSS XML\n");
		goto exit;
	}

	snprintf(msg.msg, sizeof(msg.msg), "RSS feed is done");
	msg.len = strlen(msg.msg);

	ret = mq_send(hanover_mq, (uint8_t *)&msg, sizeof(msg), 0);
	if (ret)
		ERR("Failed sending mq msg: %d\n", -errno);

exit:
	if (xml_reader)
		xmlFreeTextReader(xml_reader);

	if (hanover_mq > 0) {
		ret = mq_close(hanover_mq);
		if (ret)
			ERR("Failed closing mq: %d\n", -errno);
	}

	if (curl_handle)
		curl_easy_cleanup(curl_handle);

	if (data.data)
		free(data.data);

	curl_global_cleanup();

	return 0;
}
