/* AMR/AMR-WB SDP negotiation for mono RFC 4867 payloads.
 * Based on traud/asterisk-amr (Unlicense), ported to Asterisk 22.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "asterisk.h"
#include <ctype.h>
#include <limits.h>
#include "asterisk/module.h"
#include "asterisk/format.h"
#include "asterisk/astobj2.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "amr.h"

static const struct amr_attr defaults = { .max_red = -1 };
static int module_ready;

void amr_formats_ready(int ready)
{
    module_ready = ready;
}

static const struct amr_attr *attributes(const struct ast_format *format)
{
	const struct amr_attr *attr = ast_format_get_attribute_data(format);
	return attr ? attr : &defaults;
}

static void amr_destroy(struct ast_format *format)
{
	ast_free(ast_format_get_attribute_data(format));
}

static int amr_clone(const struct ast_format *src, struct ast_format *dst)
{
	struct amr_attr *attr = ast_malloc(sizeof(*attr));
	if (!attr) {
		return -1;
	}
	*attr = *attributes(src);
	ast_format_set_attribute_data(dst, attr);
	return 0;
}

static int number(const char *value, unsigned int maximum, unsigned int *out)
{
	char *end;
	unsigned long n;
	if (!isdigit((unsigned char) *value)) {
		return -1;
	}
	errno = 0;
	n = strtoul(value, &end, 10);
	if (errno || *end || n > maximum) {
		return -1;
	}
	*out = n;
	return 0;
}

static struct ast_format *amr_parse_sdp_fmtp(const struct ast_format *format, const char *text)
{
	struct ast_format *result;
	struct amr_attr parsed = defaults;
	char *copy, *rest, *item;
	unsigned int seen = 0;
	unsigned int last_mode = ast_format_get_sample_rate(format) == 16000 ? 8 : 7;
	if (!module_ready || !text || strlen(text) > 1024 || ast_format_get_channel_count(format) > 1) {
		return NULL;
	}
	copy = ast_strdup(text);
	if (!copy) {
		return NULL;
	}
	rest = copy;
	while ((item = strsep(&rest, ";"))) {
		char *key = ast_strip(item), *value = strchr(key, '=');
		unsigned int n, flag = 0;
		if (!*key) {
			continue;
		}
		if (!value) {
			goto invalid;
		}
		*value++ = '\0';
		key = ast_strip(key);
		value = ast_strip(value);
		if (!strcasecmp(key, "octet-align")) {
			flag = 1;
			if (number(value, 1, &parsed.octet_align)) goto invalid;
		} else if (!strcasecmp(key, "mode-set")) {
			char *mode;
			flag = 2;
			while ((mode = strsep(&value, ","))) {
				if (number(ast_strip(mode), last_mode, &n)) goto invalid;
				parsed.mode_set |= 1U << n;
			}
		} else if (!strcasecmp(key, "mode-change-period")) {
			flag = 4;
			if (number(value, 2, &n) || !n) goto invalid;
			parsed.mode_change_period = n;
		} else if (!strcasecmp(key, "mode-change-neighbor")) {
			flag = 8;
			if (number(value, 1, &parsed.mode_change_neighbor)) goto invalid;
		} else if (!strcasecmp(key, "mode-change-capability")) {
			flag = 16;
			if (number(value, 2, &n) || !n) goto invalid;
		} else if (!strcasecmp(key, "max-red")) {
			flag = 32;
			if (number(value, INT_MAX, &n)) goto invalid;
			parsed.max_red = n;
		} else if (!strcasecmp(key, "crc") || !strcasecmp(key, "robust-sorting")) {
			flag = !strcasecmp(key, "crc") ? 64 : 128;
			if (number(value, 0, &n)) goto invalid;
		} else if (!strcasecmp(key, "interleaving")) {
			/* Presence enables an unsupported packet layout, even with value 0. */
			goto invalid;
		} else if (!strcasecmp(key, "channels")) {
			flag = 256;
			if (number(value, 1, &n) || n != 1) goto invalid;
		}
		if (seen & flag) goto invalid;
		seen |= flag;
	}
	ast_free(copy);
	result = ast_format_clone(format);
	if (result) {
		parsed.specified = 1;
		*(struct amr_attr *) ast_format_get_attribute_data(result) = parsed;
	}
	return result;
invalid:
	ast_free(copy);
	return NULL;
}

static int compatible(const struct amr_attr *a, const struct amr_attr *b)
{
	if (!a->specified || !b->specified) return 1;
	/* RFC 4867: do not silently change alignment or an explicit mode-set. */
	return a->octet_align == b->octet_align
		&& (!a->mode_set || !b->mode_set || a->mode_set == b->mode_set);
}

static enum ast_format_cmp_res amr_cmp(const struct ast_format *a, const struct ast_format *b)
{
	const struct amr_attr *aa = attributes(a), *bb = attributes(b);
	if (ast_format_get_channel_count(a) > 1 || ast_format_get_channel_count(b) > 1
		|| !compatible(aa, bb)) {
		return AST_FORMAT_CMP_NOT_EQUAL;
	}
	if (aa->octet_align != bb->octet_align || aa->mode_set != bb->mode_set
		|| aa->mode_change_period != bb->mode_change_period
		|| aa->mode_change_neighbor != bb->mode_change_neighbor) {
		return AST_FORMAT_CMP_SUBSET;
	}
	return AST_FORMAT_CMP_EQUAL;
}

static struct ast_format *amr_getjoint(const struct ast_format *a, const struct ast_format *b)
{
	const struct amr_attr *aa = attributes(a), *bb = attributes(b);
	struct ast_format *joint;
	struct amr_attr *attr;
	if (amr_cmp(a, b) == AST_FORMAT_CMP_NOT_EQUAL) return NULL;
	joint = ast_format_clone(aa->specified ? a : b);
	if (!joint) return NULL;
	attr = ast_format_get_attribute_data(joint);
	attr->mode_set = aa->mode_set ? aa->mode_set : bb->mode_set;
	attr->mode_change_period = MAX(aa->mode_change_period, bb->mode_change_period);
	attr->mode_change_neighbor = MAX(aa->mode_change_neighbor, bb->mode_change_neighbor);
	/* No references to either input are mutated. */
	return joint;
}

static void amr_generate_sdp_fmtp(const struct ast_format *format, unsigned int payload,
	struct ast_str **str)
{
	const struct amr_attr *attr = attributes(format);
	unsigned int i, listed = 0;
	ast_str_append(str, 0, "a=fmtp:%u octet-align=%u;mode-change-capability=2", payload, attr->octet_align);
	if (attr->mode_set) {
		ast_str_append(str, 0, ";mode-set=");
		for (i = 0; i < 9; ++i) {
			if (attr->mode_set & (1U << i)) {
				ast_str_append(str, 0, "%s%u", listed++ ? "," : "", i);
			}
		}
	}
	if (attr->mode_change_period) ast_str_append(str, 0, ";mode-change-period=%u", attr->mode_change_period);
	if (attr->mode_change_neighbor) ast_str_append(str, 0, ";mode-change-neighbor=1");
	if (attr->max_red >= 0) ast_str_append(str, 0, ";max-red=%d", attr->max_red);
	ast_str_append(str, 0, "\r\n");
}

static struct ast_format_interface amr_interface = {
	.format_destroy = amr_destroy,
	.format_clone = amr_clone,
	.format_cmp = amr_cmp,
	.format_get_joint = amr_getjoint,
	.format_parse_sdp_fmtp = amr_parse_sdp_fmtp,
	.format_generate_sdp_fmtp = amr_generate_sdp_fmtp,
};


int amr_register_format_interfaces(void)
{
    return ast_format_interface_register("amr", &amr_interface)
        || ast_format_interface_register("amr-wb", &amr_interface);
}
