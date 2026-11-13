#include "asterisk.h"
#include "asterisk/module.h"
#include "asterisk/res_pjsip.h"
/* Exercise the module's Contact builder with PJSIP's real parser/serializer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pjlib.h>
#include <pjlib-util.h>
#include <pjsip.h>
#include "jio_contact.h"

#define require(condition, message) do { if (!(condition)) { ast_log(LOG_ERROR, "JIO CONTACT FAIL: %s\n", message); return -1; } } while (0)

static int render(pjsip_contact_hdr *contact, char *output, size_t size)
{
	int length = pjsip_hdr_print_on(contact, output, size - 1);
	require(length > 0, "Contact serializes");
	output[length] = '\0';
	return 0;
}

static int check_contact(pj_pool_t *pool, const char *input, const char *preference)
{
	char value[512], before[2048], after[2048], again[2048];
	pj_str_t name = pj_str("Contact");
	pjsip_tx_data tdata = { 0 };
	pjsip_contact_hdr *contact;

	snprintf(value, sizeof(value), "%s", input);
	contact = pjsip_parse_hdr(pool, &name, value, strlen(value), NULL);
	require(contact != NULL, "Contact parses");
	tdata.pool = pool;
	tdata.msg = pjsip_msg_create(pool, PJSIP_REQUEST_MSG);
	pjsip_msg_add_hdr(tdata.msg, (pjsip_hdr *) contact);
	require(!render(contact, before, sizeof(before)), "render succeeds");

	jio_add_contact_features(&tdata, "911234567890@example.invalid",
		"\"<00000000-0000-1000-8000-000000000001>\"");
	require(!render(contact, after, sizeof(after)), "render succeeds");
	if (preference) {
		const char *feature = strstr(after, ";+u.jio.jfv");
		require(feature != NULL, "Jio capability is present");
		feature += strlen(";+u.jio.jfv");
		require(*feature == ';' || *feature == '\0',
			"Jio capability is a flag, not an encoded q=0.5 value");
		require(strstr(after, preference) != NULL,
			"Contact preference is a separate parameter; explicit preferences are preserved");
		require(strstr(after, ";+sip.instance=\"<00000000-0000-1000-8000-000000000001>\"") != NULL,
			"instance identity is preserved");
	} else {
		require(strcmp(before, after) == 0, "non-Jio Contact is unchanged");
	}

	jio_add_contact_features(&tdata, "911234567890@example.invalid",
		"\"<00000000-0000-1000-8000-000000000001>\"");
	require(!render(contact, again, sizeof(again)), "render succeeds");
	require(strcmp(after, again) == 0, "repeated processing does not duplicate parameters");
	return 0;
}

static int load_module(void)
{
    pj_pool_t *pool = pjsip_endpt_create_pool(ast_sip_get_pjsip_endpoint(), "jio-test", 4096, 4096);
    int failed;
    require(pool != NULL, "pool allocates");
    failed = check_contact(pool, "<sip:+911234567890@[2001:db8::1]:5060;transport=TCP>", ";q=0.5;")
        || check_contact(pool, "<sip:+911234567890@[2001:db8::1]:5060;transport=TCP>;q=0.8", ";q=0.8;")
        || check_contact(pool, "<sip:linphone@192.0.2.1:5060>", NULL)
        || check_contact(pool, "*", NULL);
    pj_pool_release(pool);
    if (failed) return AST_MODULE_LOAD_DECLINE;
    ast_log(LOG_NOTICE, "JIO CONTACT VALIDATION PASSED\n");
    return AST_MODULE_LOAD_SUCCESS;
}
static int unload_module(void) { return 0; }
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Offline Jio Contact validation",
    .load = load_module,
    .unload = unload_module,
    .requires = "res_pjsip",
    .load_pri = AST_MODPRI_CHANNEL_DEPEND,
);
