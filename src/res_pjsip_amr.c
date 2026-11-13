/* Filter unsupported AMR before PJSIP offer/answer negotiation.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "asterisk.h"
#include "asterisk/module.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/format_cache.h"
#include "asterisk/format.h"
#include <pjmedia/sdp.h>
#include <pjmedia/sdp_neg.h>
#include <pjsip_ua.h>
#include "amr.h"

static unsigned int attribute_count(const pjmedia_sdp_media *media, const char *name, const pj_str_t *pt)
{
    unsigned int i, count = 0;
    for (i = 0; i < media->attr_count; ++i) {
        if (pjmedia_sdp_attr_find2(1, &media->attr[i], name, pt)) ++count;
    }
    return count;
}

static int valid_payload(pj_pool_t *pool, pjmedia_sdp_media *media, pjmedia_sdp_rtpmap *map)
{
    struct ast_format *format, *parsed = NULL;
    pjmedia_sdp_attr *attr;
    pjmedia_sdp_fmtp fmtp;
    char params[256] = "";
    int wideband = !pj_stricmp2(&map->enc_name, "AMR-WB");
    if (attribute_count(media, "rtpmap", &map->pt) != 1
        || attribute_count(media, "fmtp", &map->pt) > 1) return 0;
    if (map->clock_rate != (wideband ? 16000 : 8000)
        || (map->param.slen && pj_strcmp2(&map->param, "1"))) return 0;
    attr = pjmedia_sdp_media_find_attr2(media, "fmtp", &map->pt);
    if (attr) {
        if (pjmedia_sdp_attr_get_fmtp(attr, &fmtp) != PJ_SUCCESS
            || fmtp.fmt_param.slen < 0 || (size_t) fmtp.fmt_param.slen >= sizeof(params)) return 0;
        ast_copy_pj_str(params, &fmtp.fmt_param, sizeof(params));
    }
    format = ast_format_cache_get(wideband ? "amrwb" : "amr");
    if (format) parsed = ast_format_parse_sdp_fmtp(format, params);
    ao2_cleanup(format);
    if (!parsed) return 0;
    ao2_cleanup(parsed);
    if (!attr) {
        /* Force stock get_codecs() to parse the RFC 4867 wire defaults. */
        char value[48];
        pj_str_t text;
        if (media->attr_count == PJMEDIA_MAX_SDP_ATTR) return 0;
        snprintf(value, sizeof(value), "%.*s octet-align=0", (int) map->pt.slen, map->pt.ptr);
        text = pj_str(value);
        attr = pjmedia_sdp_attr_create(pool, "fmtp", &text);
        pjmedia_sdp_media_add_attr(media, attr);
    }
    return 1;
}

static struct ast_format *parse_media_format(pj_pool_t *pool, pjmedia_sdp_media *media, unsigned int index)
{
    pjmedia_sdp_attr *attr;
    pjmedia_sdp_rtpmap *map;
    pjmedia_sdp_fmtp fmtp;
    struct ast_format *format, *parsed;
    char params[256] = "";
    attr = pjmedia_sdp_media_find_attr2(media, "rtpmap", &media->desc.fmt[index]);
    if (!attr || pjmedia_sdp_attr_to_rtpmap(pool, attr, &map) != PJ_SUCCESS
        || !valid_payload(pool, media, map)) return NULL;
    attr = pjmedia_sdp_media_find_attr2(media, "fmtp", &map->pt);
    if (attr) {
        if (pjmedia_sdp_attr_get_fmtp(attr, &fmtp) != PJ_SUCCESS
            || fmtp.fmt_param.slen < 0 || (size_t) fmtp.fmt_param.slen >= sizeof(params)) return NULL;
        ast_copy_pj_str(params, &fmtp.fmt_param, sizeof(params));
    }
    format = ast_format_cache_get(!pj_stricmp2(&map->enc_name, "AMR-WB") ? "amrwb" : "amr");
    parsed = format ? ast_format_parse_sdp_fmtp(format, params) : NULL;
    ao2_cleanup(format);
    return parsed;
}

/* PJPROJECT otherwise compares only rtpmap, allowing an incompatible answer
 * to change the packet layout. Both parsed formats here describe actual SDP,
 * unlike the codec module's wildcard local capability. RFC 4867 section 8.3.1. */
static pj_status_t match_offer_answer(pj_pool_t *pool,
    pjmedia_sdp_media *offer, unsigned int oi, pjmedia_sdp_media *answer,
    unsigned int ai, unsigned int options)
{
    struct ast_format *o = parse_media_format(pool, offer, oi);
    struct ast_format *a = parse_media_format(pool, answer, ai);
    pj_status_t result = PJ_EINVAL;
    if (o && a) {
        const struct amr_attr *oa = ast_format_get_attribute_data(o);
        const struct amr_attr *aa = ast_format_get_attribute_data(a);
        if (oa->octet_align == aa->octet_align
            && (!oa->mode_set || oa->mode_set == aa->mode_set)) result = PJ_SUCCESS;
    }
    ao2_cleanup(o);
    ao2_cleanup(a);
    return result;
}

static pj_bool_t on_rx_message(pjsip_rx_data *rdata)
{
    pjsip_rdata_sdp_info *info;
    unsigned int m, i;
    if (!rdata->msg_info.msg->body) return PJ_FALSE;
    info = pjsip_rdata_get_sdp_info(rdata);
    if (!info || info->sdp_err != PJ_SUCCESS || !info->sdp) return PJ_FALSE;
    for (m = 0; m < info->sdp->media_count; ++m) {
        pjmedia_sdp_media *media = info->sdp->media[m];
        unsigned int retained = 0;
        pj_str_t original;
        if (pj_stricmp2(&media->desc.media, "audio") || !media->desc.port || !media->desc.fmt_count) continue;
        original = media->desc.fmt[0];
        for (i = 0; i < media->desc.fmt_count; ++i) {
            pjmedia_sdp_attr *attr = pjmedia_sdp_media_find_attr2(media, "rtpmap", &media->desc.fmt[i]);
            pjmedia_sdp_rtpmap *map;
            if (attr && pjmedia_sdp_attr_to_rtpmap(rdata->tp_info.pool, attr, &map) == PJ_SUCCESS
                && (!pj_stricmp2(&map->enc_name, "AMR") || !pj_stricmp2(&map->enc_name, "AMR-WB"))
                && !valid_payload(rdata->tp_info.pool, media, map)) {
                continue;
            }
            media->desc.fmt[retained++] = media->desc.fmt[i];
        }
        media->desc.fmt_count = retained;
        if (!retained) {
            /* Keep structurally valid SDP and let the normal session reject it. */
            media->desc.port = 0;
            media->desc.fmt_count = 1;
            media->desc.fmt[0] = original;
        }
    }
    return PJ_FALSE;
}

static pjsip_module guard = {
    .name = { "AMR SDP guard", 13 },
    .id = -1,
    .priority = PJSIP_MOD_PRIORITY_TSX_LAYER - 1,
    .on_rx_request = on_rx_message,
    .on_rx_response = on_rx_message,
};

static int load_module(void)
{
    static const pj_str_t nb = { "AMR", 3 }, wb = { "AMR-WB", 6 };
    const char *names[] = { "amr", "amrwb" };
    unsigned int i;
    for (i = 0; i < ARRAY_LEN(names); ++i) {
        struct ast_format *format = ast_format_cache_get(names[i]);
        struct ast_format *parsed = format ? ast_format_parse_sdp_fmtp(format, "") : NULL;
        int ready = parsed != NULL;
        ao2_cleanup(format);
        ao2_cleanup(parsed);
        if (!ready) {
            ast_log(LOG_ERROR, "AMR codec initialization incomplete; restart Asterisk after correcting the error\n");
            return AST_MODULE_LOAD_DECLINE;
        }
    }
    if (pjmedia_sdp_neg_register_fmt_match_cb(&nb, match_offer_answer) != PJ_SUCCESS) return AST_MODULE_LOAD_DECLINE;
    if (pjmedia_sdp_neg_register_fmt_match_cb(&wb, match_offer_answer) != PJ_SUCCESS) {
        pjmedia_sdp_neg_register_fmt_match_cb(&nb, NULL);
        return AST_MODULE_LOAD_DECLINE;
    }
    if (ast_sip_register_service(&guard)) {
        pjmedia_sdp_neg_register_fmt_match_cb(&nb, NULL);
        pjmedia_sdp_neg_register_fmt_match_cb(&wb, NULL);
        return AST_MODULE_LOAD_DECLINE;
    }
    /* Existing dialogs must never lose their AMR validation callbacks. */
    ast_module_shutdown_ref(AST_MODULE_SELF);
    return AST_MODULE_LOAD_SUCCESS;
}
static int unload_module(void) { return -1; }
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "AMR SDP validation",
    .load = load_module,
    .unload = unload_module,
    .load_pri = AST_MODPRI_CHANNEL_DEPEND - 1,
    .requires = "res_pjsip,codec_amr",
);
