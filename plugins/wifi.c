/* SPDX-License-Identifier: MIT */
/* Status bar item: whether wlan0 has a link. */
#include <stdio.h>
#include <string.h>
#include "starry-plugin.h"

static void wifi_text(void *data, char *buffer, size_t size) {
	char state[16] = "";
	FILE *file = fopen("/sys/class/net/wlan0/operstate", "r");
	if (file) {
		if (fscanf(file, "%15s", state) != 1) {
			state[0] = 0;
		}
		fclose(file);
	}
	snprintf(buffer, size, "%s", strcmp(state, "up") == 0 ? "Wi-Fi" : "No Wi-Fi");
}

static bool wifi_init(struct starry_host *host, const struct starry_host_api *api) {
	const struct starry_status_item item = {
		.size = sizeof(item),
		.id = "wifi",
		.side = STARRY_STATUS_RIGHT,
		.refresh_msec = 2000,
		.text = wifi_text,
	};
	return api->add_status_item(host, &item);
}

const struct starry_plugin starry_plugin = {
	.abi = STARRY_PLUGIN_ABI,
	.name = "wifi",
	.init = wifi_init,
};
