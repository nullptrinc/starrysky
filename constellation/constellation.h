/* SPDX-License-Identifier: MIT */
/*
 * Constellation: Starry's hardware abstraction, in the spirit of QNX PPS on
 * BlackBerry 10. System services are the only code that touches drivers.
 * They publish state as small objects in the /blinks namespace through the
 * constellationd broker; applications read objects and subscribe to them.
 *
 * An object is a path and a set of typed attributes, written as text:
 *
 *     @/blinks/power/battery
 *     level:n:64
 *     status::charging
 *     charging:b:true
 *
 * "name:type:value" per line; type "n" is a number, "b" a boolean, empty a
 * string. Publishing merges attributes into the object. The connection that
 * creates an object owns it: only it may change or delete the object until
 * it disconnects. Subscribers get the whole object whenever it changes; a
 * slow subscriber misses intermediate states but always gets the latest one.
 */
#ifndef CONSTELLATION_H
#define CONSTELLATION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CN_SOCKET "/run/constellation.sock" /* $CONSTELLATION_SOCKET overrides */
#define CN_NAMESPACE "/blinks/"
#define CN_PATH_MAX 128
#define CN_NAME_MAX 48
#define CN_VALUE_MAX 208
#define CN_MAX_ATTRS 32
#define CN_MESSAGE_MAX 16384

#define CN_STRING 's'
#define CN_NUMBER 'n'
#define CN_BOOL 'b'
#define CN_REMOVED '-' /* only in a published object: deletes the attribute */

struct cn_attr {
	char name[CN_NAME_MAX];
	char type;
	char value[CN_VALUE_MAX];
};

struct cn_object {
	char path[CN_PATH_MAX];
	struct cn_attr attrs[CN_MAX_ATTRS];
	int count;
};

/* ---- objects ---- */

void cn_object_init(struct cn_object *object, const char *path);
/* Setters replace an attribute of the same name; newlines become spaces. */
void cn_set_string(struct cn_object *object, const char *name, const char *value);
void cn_set_int(struct cn_object *object, const char *name, long value);
void cn_set_number(struct cn_object *object, const char *name, double value);
void cn_set_bool(struct cn_object *object, const char *name, bool value);
void cn_set_removed(struct cn_object *object, const char *name);
const struct cn_attr *cn_find_attr(const struct cn_object *object, const char *name);
/* The value of a named attribute, or the fallback when it is missing. */
const char *cn_get(const struct cn_object *object, const char *name, const char *fallback);
long cn_get_int(const struct cn_object *object, const char *name, long fallback);
double cn_get_number(const struct cn_object *object, const char *name, double fallback);
bool cn_get_bool(const struct cn_object *object, const char *name, bool fallback);

/* True for "/blinks/..." paths of allowed characters; prefix allows a trailing "/". */
bool cn_valid_path(const char *path, bool prefix);
bool cn_valid_name(const char *name);
/* Wire format: the attribute lines after the header line. */
int cn_format_attrs(const struct cn_object *object, char *buffer, int size);
bool cn_parse_attrs(const char *text, struct cn_object *object);

/* ---- the broker connection ---- */

struct cn_client;
struct cn_client *cn_connect(void);
void cn_disconnect(struct cn_client *client);
/* Poll this for readability, then call cn_dispatch. */
int cn_fd(const struct cn_client *client);
/* Connects again after the broker went away and repeats the subscriptions. */
bool cn_reconnect(struct cn_client *client);

/* Merges the object's attributes into the published one, creating it if needed. */
bool cn_publish(struct cn_client *client, const struct cn_object *object);
bool cn_delete(struct cn_client *client, const char *path);
/* 1 and the object when it exists, 0 when it does not, -1 on error. */
int cn_fetch(struct cn_client *client, const char *path, struct cn_object *object);
/* Calls each() for every object under prefix; returns their number or -1. */
int cn_list(struct cn_client *client, const char *prefix,
	void (*each)(const char *path, void *data), void *data);

/* A path ending in "/" subscribes to everything under it. The current state
 * of matching objects arrives right away, through cn_dispatch. */
bool cn_subscribe(struct cn_client *client, const char *path);
bool cn_unsubscribe(struct cn_client *client, const char *path);

/* exists is false when the object was deleted or never published. */
typedef void (*cn_changed_fn)(const struct cn_object *object, bool exists, void *data);
/* Delivers pending changes without blocking; false once the broker is gone. */
bool cn_dispatch(struct cn_client *client, cn_changed_fn changed, void *data);

#ifdef __cplusplus
}
#endif

#endif
