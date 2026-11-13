/* Jio SIP compatibility. Licensed under GNU GPL version 2; see LICENSE. */
/*** MODULEINFO
	<depend>pjproject</depend>
	<depend>res_pjsip</depend>
	<support_level>extended</support_level>
 ***/
#include "asterisk.h"
#include <pjsip.h>
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/res_pjsip.h"
#include "jio_contact.h"

/* Immutable while registered: configuration changes require unload/load. */
static char *realm;
static char *username;
static char *digest_uri;
static char *access_network;
static char *instance_param;

static int matches_uri(pjsip_uri *uri)
{
	pjsip_sip_uri *sip;
	if (!uri || (!PJSIP_URI_SCHEME_IS_SIP(uri) && !PJSIP_URI_SCHEME_IS_SIPS(uri))) {
		return 0;
	}
	sip = pjsip_uri_get_uri(uri);
	return !pj_stricmp2(&sip->host, realm);
}

static pj_status_t on_tx_response(pjsip_tx_data *tdata)
{
	jio_add_contact_features(tdata, username, instance_param);
	return PJ_SUCCESS;
}

static pj_status_t on_tx_request(pjsip_tx_data *tdata)
{
	pjsip_authorization_hdr *auth;
	pj_str_t name = pj_str("P-Access-Network-Info");
	pj_str_t value = pj_str(access_network);
	jio_add_contact_features(tdata, username, instance_param);
	/* 3GPP TS 24.229: P-Access-Network-Info must appear in every outbound
	 * SIP request from the UE, not only REGISTER. */
	if (!pjsip_msg_find_hdr_by_name(tdata->msg, &name, NULL)) {
		pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr *)
			pjsip_generic_string_hdr_create(tdata->pool, &name, &value));
		pjsip_tx_data_invalidate_msg(tdata);
	}
	if (tdata->msg->line.req.method.id != PJSIP_REGISTER_METHOD
		|| !matches_uri(tdata->msg->line.req.uri)) {
		return PJ_SUCCESS;
	}
	/* Preserve a real challenge response and avoid duplicates on retransmission. */
	if (!pjsip_msg_find_hdr(tdata->msg, PJSIP_H_AUTHORIZATION, NULL)) {
		auth = pjsip_authorization_hdr_create(tdata->pool);
		pj_strdup2(tdata->pool, &auth->scheme, "Digest");
		pj_strdup2(tdata->pool, &auth->credential.digest.username, username);
		pj_strdup2(tdata->pool, &auth->credential.digest.realm, realm);
		pj_strdup2(tdata->pool, &auth->credential.digest.uri, digest_uri);
		pj_strdup2(tdata->pool, &auth->credential.digest.nonce, "");
		pj_strdup2(tdata->pool, &auth->credential.digest.response, "");
		pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr *) auth);
		pjsip_tx_data_invalidate_msg(tdata);
	}
	return PJ_SUCCESS;
}

static pj_bool_t on_rx_response(pjsip_rx_data *rdata)
{
	pjsip_www_authenticate_hdr *challenge;
	pjsip_msg *msg = rdata->msg_info.msg;
	if (msg->line.status.code != PJSIP_SC_TOO_MANY_HOPS
		|| !rdata->msg_info.from || !matches_uri(rdata->msg_info.from->uri)) {
		return PJ_FALSE;
	}
	challenge = pjsip_msg_find_hdr(msg, PJSIP_H_WWW_AUTHENTICATE, NULL);
	if (challenge && !pj_stricmp2(&challenge->scheme, "Digest")
		&& !pj_stricmp2(&challenge->challenge.digest.realm, realm)) {
		/* Run before the transaction layer so stock auth paths see a challenge. */
		msg->line.status.code = PJSIP_SC_UNAUTHORIZED;
		msg->line.status.reason = *pjsip_get_status_text(PJSIP_SC_UNAUTHORIZED);
	}
	return PJ_FALSE;
}

static pjsip_module jio_module = {
	.name = { "Jio compatibility", 17 },
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_TSX_LAYER - 3,
	.on_tx_request = on_tx_request,
	.on_tx_response = on_tx_response,
	.on_rx_response = on_rx_response,
};

static void free_config(void)
{
	ast_free(realm);
	ast_free(username);
	ast_free(digest_uri);
	ast_free(access_network);
	ast_free(instance_param);
	realm = username = digest_uri = access_network = instance_param = NULL;
}

static int load_module(void)
{
	struct ast_flags flags = { 0 };
	struct ast_config *cfg = ast_config_load("pjsip_jio.conf", flags);
	const char *r, *u, *d, *a, *i;
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID || cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return AST_MODULE_LOAD_DECLINE;
	}
	r = ast_variable_retrieve(cfg, "general", "realm");
	u = ast_variable_retrieve(cfg, "general", "username");
	d = ast_variable_retrieve(cfg, "general", "digest_uri");
	a = ast_variable_retrieve(cfg, "general", "access_network");
	if (ast_strlen_zero(r) || ast_strlen_zero(u) || ast_strlen_zero(d) || ast_strlen_zero(a)) {
		ast_log(LOG_ERROR, "pjsip_jio.conf requires realm, username, digest_uri and access_network\n");
		ast_config_destroy(cfg);
		return AST_MODULE_LOAD_DECLINE;
	}
	realm = ast_strdup(r);
	username = ast_strdup(u);
	digest_uri = ast_strdup(d);
	access_network = ast_strdup(a);
	/* Optional: bare UUID, emitted as +sip.instance="<UUID>". */
	i = ast_variable_retrieve(cfg, "general", "instance_id");
	if (!ast_strlen_zero(i)) {
		ast_asprintf(&instance_param, "\"<%s>\"", i);
	}
	ast_config_destroy(cfg);
	if (!realm || !username || !digest_uri || !access_network
		|| ast_sip_register_service(&jio_module)) {
		free_config();
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_sip_unregister_service(&jio_module);
	free_config();
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Jio SIP compatibility",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND - 1,
	.requires = "res_pjsip",
);
