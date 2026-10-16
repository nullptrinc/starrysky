/* SPDX-License-Identifier: MIT */
/* Constellation objects and their text form, shared by the broker and clients. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "constellation.h"

void cn_object_init(struct cn_object *object, const char *path) {
	object->count = 0;
	snprintf(object->path, sizeof(object->path), "%s", path);
}

static struct cn_attr *slot(struct cn_object *object, const char *name) {
	for (int i = 0; i < object->count; i++) {
		if (strcmp(object->attrs[i].name, name) == 0) {
			return &object->attrs[i];
		}
	}
	if (object->count >= CN_MAX_ATTRS) {
		return NULL;
	}
	struct cn_attr *attr = &object->attrs[object->count++];
	snprintf(attr->name, sizeof(attr->name), "%s", name);
	return attr;
}

static void set(struct cn_object *object, const char *name, char type, const char *value) {
	struct cn_attr *attr = slot(object, name);
	if (!attr) {
		return;
	}
	attr->type = type;
	snprintf(attr->value, sizeof(attr->value), "%s", value);
	for (char *c = attr->value; *c; c++) {
		if (*c == '\n' || *c == '\r') {
			*c = ' ';
		}
	}
}

void cn_set_string(struct cn_object *object, const char *name, const char *value) {
	set(object, name, CN_STRING, value);
}

void cn_set_int(struct cn_object *object, const char *name, long value) {
	char text[32];
	snprintf(text, sizeof(text), "%ld", value);
	set(object, name, CN_NUMBER, text);
}

void cn_set_number(struct cn_object *object, const char *name, double value) {
	char text[32];
	snprintf(text, sizeof(text), "%.9g", value);
	set(object, name, CN_NUMBER, text);
}

void cn_set_bool(struct cn_object *object, const char *name, bool value) {
	set(object, name, CN_BOOL, value ? "true" : "false");
}

void cn_set_removed(struct cn_object *object, const char *name) {
	set(object, name, CN_REMOVED, "");
}

const struct cn_attr *cn_find_attr(const struct cn_object *object, const char *name) {
	for (int i = 0; i < object->count; i++) {
		if (strcmp(object->attrs[i].name, name) == 0 && object->attrs[i].type != CN_REMOVED) {
			return &object->attrs[i];
		}
	}
	return NULL;
}

const char *cn_get(const struct cn_object *object, const char *name, const char *fallback) {
	const struct cn_attr *attr = cn_find_attr(object, name);
	return attr ? attr->value : fallback;
}

long cn_get_int(const struct cn_object *object, const char *name, long fallback) {
	const struct cn_attr *attr = cn_find_attr(object, name);
	char *end;
	double value = attr ? strtod(attr->value, &end) : 0;
	return attr && end != attr->value ? (long)value : fallback;
}

double cn_get_number(const struct cn_object *object, const char *name, double fallback) {
	const struct cn_attr *attr = cn_find_attr(object, name);
	char *end;
	double value = attr ? strtod(attr->value, &end) : 0;
	return attr && end != attr->value ? value : fallback;
}

bool cn_get_bool(const struct cn_object *object, const char *name, bool fallback) {
	const struct cn_attr *attr = cn_find_attr(object, name);
	if (!attr) {
		return fallback;
	}
	return strcmp(attr->value, "true") == 0 || strcmp(attr->value, "1") == 0;
}

static bool name_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		c == '_' || c == '-' || c == '.';
}

bool cn_valid_name(const char *name) {
	size_t length = strlen(name);
	if (length == 0 || length >= CN_NAME_MAX) {
		return false;
	}
	for (const char *c = name; *c; c++) {
		if (!name_char(*c)) {
			return false;
		}
	}
	return true;
}

bool cn_valid_path(const char *path, bool prefix) {
	size_t length = strlen(path);
	size_t root = strlen(CN_NAMESPACE);
	if (length >= CN_PATH_MAX || strncmp(path, CN_NAMESPACE, root) != 0) {
		return false;
	}
	if (length == root) {
		return prefix; /* the whole namespace */
	}
	/* Components of name characters, no empty or dot-only ones. */
	const char *component = path + root;
	for (const char *c = component;; c++) {
		if (*c == '/' || *c == 0) {
			size_t size = (size_t)(c - component);
			if (size == 0) {
				return *c == 0 && prefix && c[-1] == '/';
			}
			if (strspn(component, ".") >= size) {
				return false;
			}
			if (*c == 0) {
				return true;
			}
			component = c + 1;
		} else if (!name_char(*c)) {
			return false;
		}
	}
}

int cn_format_attrs(const struct cn_object *object, char *buffer, int size) {
	int length = 0;
	for (int i = 0; i < object->count && length < size; i++) {
		const struct cn_attr *attr = &object->attrs[i];
		if (attr->type == CN_REMOVED) {
			length += snprintf(buffer + length, size - length, "-%s\n", attr->name);
		} else {
			length += snprintf(buffer + length, size - length, "%s:%s:%s\n", attr->name,
				attr->type == CN_STRING ? "" : (char[]){ attr->type, 0 }, attr->value);
		}
	}
	return length < size ? length : -1;
}

bool cn_parse_attrs(const char *text, struct cn_object *object) {
	object->count = 0;
	while (*text) {
		const char *end = strchr(text, '\n');
		size_t length = end ? (size_t)(end - text) : strlen(text);
		char line[CN_NAME_MAX + CN_VALUE_MAX + 4];
		if (length >= sizeof(line)) {
			return false;
		}
		memcpy(line, text, length);
		line[length] = 0;
		text += length + (end ? 1 : 0);
		if (length == 0) {
			continue;
		}
		char type;
		const char *value = "";
		char *name = line;
		if (line[0] == '-') {
			name++;
			type = CN_REMOVED;
		} else {
			char *colon = strchr(line, ':');
			if (!colon) {
				return false;
			}
			*colon = 0;
			char *second = strchr(colon + 1, ':');
			if (!second || second - colon > 2) {
				return false;
			}
			type = second == colon + 1 ? CN_STRING : colon[1];
			value = second + 1;
			if ((type != CN_NUMBER && type != CN_BOOL && type != CN_STRING) ||
					strlen(value) >= CN_VALUE_MAX) {
				return false;
			}
		}
		if (!cn_valid_name(name) || object->count >= CN_MAX_ATTRS) {
			return false;
		}
		struct cn_attr *attr = slot(object, name);
		attr->type = type;
		snprintf(attr->value, sizeof(attr->value), "%s", value);
	}
	return true;
}
