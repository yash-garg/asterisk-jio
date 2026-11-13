/* Jio Contact construction, shared by the module and its serialization test. */
#ifndef JIO_CONTACT_H
#define JIO_CONTACT_H

#include <string.h>
#include <pjsip.h>

/* IMS feature tags that Jio's IMS core requires in every Contact.
 * Without the full RCS/MMTEL set the core does not fork inbound calls to us.
 * +u.jio.jfv is the JioFiber-Voice capability required for outbound (direct IMS). */
static const char * const jio_contact_tags[] = {
	"+g.3gpp.icsi-ref",
	"+g.3gpp.iari-ref",
	"+g.gsma.rcs.telephony",
	"video",
	"+u.jio.jfv",
	"+sip.instance",
	NULL,
};
static const char * const jio_contact_tag_values[] = {
	"\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mmtel\"",
	"\"urn%3Aurn-7%3A3gpp-application.ims.iari.rcs.jio.eucr\"",
	"\"none\"",
	"",   /* video: no value */
	"",   /* +u.jio.jfv is a capability flag, not the Contact preference */
	NULL,  /* +sip.instance value filled from instance_param at runtime */
};

/* The Jio CPE identifies itself with +sip.instance and IMS feature tags in
 * every Contact it sends. Only Contacts whose user part is our line number
 * (user part of username, with a leading '+') are touched. */
static void jio_add_contact_features(pjsip_tx_data *tdata,
	const char *username, const char *instance_param)
{
	pjsip_contact_hdr *contact;
	pjsip_sip_uri *sip;
	pjsip_param *param;
	const char *at = strchr(username, '@');
	pj_ssize_t len = at ? at - username : (pj_ssize_t) strlen(username);
	int i;

	if (!instance_param) {
		return;
	}
	contact = pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CONTACT, NULL);
	if (!contact || contact->star || !contact->uri
		|| (!PJSIP_URI_SCHEME_IS_SIP(contact->uri) && !PJSIP_URI_SCHEME_IS_SIPS(contact->uri))) {
		return;
	}
	sip = pjsip_uri_get_uri(contact->uri);
	if (sip->user.slen != len + 1 || sip->user.ptr[0] != '+'
		|| pj_ansi_strnicmp(sip->user.ptr + 1, username, len)) {
		return;
	}
	for (i = 0; jio_contact_tags[i]; i++) {
		pj_str_t name = pj_str((char *) jio_contact_tags[i]);
		const char *val = (i == 5) ? instance_param : jio_contact_tag_values[i];
		if (pjsip_param_find(&contact->other_param, &name)) {
			continue; /* already present, skip */
		}
		param = PJ_POOL_ZALLOC_T(tdata->pool, pjsip_param);
		pj_strdup(tdata->pool, &param->name, &name);
		pj_strdup2(tdata->pool, &param->value, val);
		pj_list_push_back(&contact->other_param, param);
	}
	/* q is a Contact parameter, not a value of +u.jio.jfv. PJSIP stores
	 * it in thousandths; putting "q=0.5" in a feature value escapes '='. */
	if (!contact->q1000) {
		contact->q1000 = 500;
	}
	pjsip_tx_data_invalidate_msg(tdata);
}

#endif
