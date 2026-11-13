/* Based on traud/asterisk-amr 420ab33f (Unlicense), ported to Asterisk 22.
 * Bounds-checked mono RFC 4867 payloads and multi-frame decoding.
 * SPDX-License-Identifier: GPL-2.0-only
 */
/*** MODULEINFO
	 <depend>amr_nb</depend>
	 <depend>amr_wb_decoder</depend>
	 <depend>amr_wb_encoder</depend>
***/

#include "asterisk.h"


#include "asterisk/codec.h"             /* for AST_MEDIA_TYPE_AUDIO */
#include "asterisk/format.h"            /* for ast_format_get_attribute_data */
#include "asterisk/frame.h"             /* for ast_frame, etc */
#include "asterisk/linkedlists.h"       /* for AST_LIST_NEXT, etc */
#include "asterisk/logger.h"            /* for ast_log, ast_debug, etc */
#include "asterisk/module.h"
#include "asterisk/translate.h"         /* for ast_trans_pvt, etc */

#include <opencore-amrnb/interf_dec.h>
#include <opencore-amrnb/interf_enc.h>
#include <opencore-amrwb/dec_if.h>
#include <vo-amrwbenc/enc_if.h>

#include "amr.h"
#include "amr_payload.h"

#define BUFFER_SAMPLES 16000 /* 1000 milliseconds */

/* Sample frame data */
#include "asterisk/slin.h"
#include "asterisk/format_cache.h"
#include "asterisk/rtp_engine.h"

static struct ast_format *addon_amr;
static struct ast_format *addon_amrwb;
#include "ex_amr.h"

struct amr_coder_pvt {
	void *state; /* May be encoder or decoder */
	unsigned int mode;
	int16_t buf[BUFFER_SAMPLES];
};

static int lintoamr_new(struct ast_trans_pvt *pvt)
{
	struct amr_coder_pvt *apvt = pvt->pvt;
	const unsigned int sample_rate = pvt->t->src_codec.sample_rate;

	struct amr_attr *attr = pvt->explicit_dst ? ast_format_get_attribute_data(pvt->explicit_dst) : NULL;
	const int dtx = 0;

	if (8000 == sample_rate) {
		apvt->state = Encoder_Interface_init(dtx);
	} else if (16000 == sample_rate) {
		apvt->state = E_IF_init();
	}

	if (!apvt->state) {
		ast_log(LOG_ERROR, "Error creating the AMR encoder for %d\n", sample_rate);
		return -1;
	}

	/* Use the lowest permitted rate. This avoids exceeding a gateway's
	 * requested rate without sharing mutable format state between calls. */
	apvt->mode = 0;
	if (attr && attr->mode_set) {
		while (!(attr->mode_set & (1U << apvt->mode))) ++apvt->mode;
	}
	ast_debug(3, "Created encoder (%d -> AMR) %p (Format %p)\n", sample_rate, apvt, pvt->explicit_dst);

	return 0;
}

static int amrtolin_new(struct ast_trans_pvt *pvt)
{
	struct amr_coder_pvt *apvt = pvt->pvt;
	const unsigned int sample_rate = pvt->t->dst_codec.sample_rate;

	if (8000 == sample_rate) {
		apvt->state = Decoder_Interface_init();
	} else if (16000 == sample_rate) {
		apvt->state = D_IF_init();
	}

	if (!apvt->state) {
		ast_log(LOG_ERROR, "Error creating the AMR decoder for %d\n", sample_rate);
		return -1;
	}

	ast_debug(3, "Created decoder (AMR -> %d) %p\n", sample_rate, apvt);

	return 0;
}

static int lintoamr_framein(struct ast_trans_pvt *pvt, struct ast_frame *f)
{
	struct amr_coder_pvt *apvt = pvt->pvt;

	if (f->samples < 0 || f->datalen < 0 || !f->data.ptr
		|| f->samples > BUFFER_SAMPLES - pvt->samples
		|| (size_t) f->datalen != (size_t) f->samples * sizeof(int16_t)) {
		return -1;
	}
	memcpy(apvt->buf + pvt->samples, f->data.ptr, f->datalen);
	pvt->samples += f->samples;

	return 0;
}

static struct ast_frame *lintoamr_frameout(struct ast_trans_pvt *pvt)
{
	struct amr_coder_pvt *apvt = pvt->pvt;
	const int wideband = pvt->t->src_codec.sample_rate == 16000;
	const unsigned int frame_size = wideband ? 320 : 160;
	const struct amr_attr *attr = ast_format_get_attribute_data(pvt->f.subclass.format);
	struct ast_frame *result = NULL, *last = NULL;
	unsigned int consumed = 0;

	while (pvt->samples >= (int) frame_size) {
		unsigned char storage[AMR_STORAGE_BYTES];
		struct ast_frame *current;
		int len;
		if (wideband) {
			len = E_IF_encode(apvt->state, apvt->mode, apvt->buf + consumed, storage, 0);
		} else {
			len = Encoder_Interface_Encode(apvt->state, apvt->mode,
				apvt->buf + consumed, storage, 0);
		}
		consumed += frame_size;
		pvt->samples -= frame_size;
		if (len <= 0 || (size_t) len > sizeof(storage)) continue;
		len = amr_payload_pack(storage, len, wideband, attr ? attr->octet_align : 0,
			pvt->outbuf.uc, pvt->t->buf_size);
		if (len < 0) continue;
		current = ast_trans_frameout(pvt, len, frame_size);
		if (!current) continue;
		if (last) {
			AST_LIST_NEXT(last, frame_list) = current;
		} else {
			result = current;
		}
		last = current;
	}
	if (consumed) {
		memmove(apvt->buf, apvt->buf + consumed, pvt->samples * sizeof(int16_t));
	}
	return result;
}

static int amrtolin_framein(struct ast_trans_pvt *pvt, struct ast_frame *f)
{
	struct amr_coder_pvt *apvt = pvt->pvt;
	const int wideband = pvt->t->dst_codec.sample_rate == 16000;
	const unsigned int frame_size = wideband ? 320 : 160;
	const struct amr_attr *attr = ast_format_get_attribute_data(f->subclass.format);
	struct amr_packet packet;
	unsigned int i;

	if (f->datalen <= 0 || amr_payload_parse(f->data.ptr, f->datalen,
		wideband, attr ? attr->octet_align : 0, &packet)) {
		return -1;
	}
	if ((int) (packet.count * frame_size) > pvt->t->buffer_samples - pvt->samples) {
		return -1;
	}
	for (i = 0; i < packet.count; ++i) {
		unsigned char storage[AMR_STORAGE_BYTES];
		int bad;
		amr_payload_frame(f->data.ptr, &packet, i, storage);
		bad = !(storage[0] & 4);
		if (wideband) {
			D_IF_decode(apvt->state, storage, pvt->outbuf.i16 + pvt->samples, bad);
		} else {
			Decoder_Interface_Decode(apvt->state, storage, pvt->outbuf.i16 + pvt->samples, bad);
		}
		pvt->samples += frame_size;
		pvt->datalen += frame_size * sizeof(int16_t);
	}
	return 0;
}

static void lintoamr_destroy(struct ast_trans_pvt *pvt)
{
	struct amr_coder_pvt *apvt = pvt->pvt;
	const unsigned int sample_rate = pvt->t->src_codec.sample_rate;

	if (!apvt || !apvt->state) {
		return;
	}

	if (8000 == sample_rate) {
		Encoder_Interface_exit(apvt->state);
	} else if (16000 == sample_rate) {
		E_IF_exit(apvt->state);
	}
	apvt->state = NULL;

	ast_debug(3, "Destroyed encoder (%d -> AMR) %p\n", sample_rate, apvt);
}

static void amrtolin_destroy(struct ast_trans_pvt *pvt)
{
	struct amr_coder_pvt *apvt = pvt->pvt;
	const unsigned int sample_rate = pvt->t->dst_codec.sample_rate;

	if (!apvt || !apvt->state) {
		return;
	}

	if (8000 == sample_rate) {
		Decoder_Interface_exit(apvt->state);
	} else if (16000 == sample_rate) {
		D_IF_exit(apvt->state);
	}
	apvt->state = NULL;

	ast_debug(3, "Destroyed decoder (AMR -> %d) %p\n", sample_rate, apvt);
}

static struct ast_translator amrtolin = {
	.name = "amrtolin",
	.src_codec = {
		.name = "amr",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 8000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 8000,
	},
	.format = "slin",
	.newpvt = amrtolin_new,
	.framein = amrtolin_framein,
	.destroy = amrtolin_destroy,
	.sample = amr_sample,
	.desc_size = sizeof(struct amr_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES / 2,
	.buf_size = BUFFER_SAMPLES,
};

static struct ast_translator lintoamr = {
	.name = "lintoamr",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 8000,
	},
	.dst_codec = {
		.name = "amr",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 8000,
	},
	.format = "amr",
	.newpvt = lintoamr_new,
	.framein = lintoamr_framein,
	.frameout = lintoamr_frameout,
	.destroy = lintoamr_destroy,
	.sample = slin8_sample,
	.desc_size = sizeof(struct amr_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES / 2,
	.buf_size = BUFFER_SAMPLES,
};

static struct ast_translator amrtolin16 = {
	.name = "amrtolin16",
	.src_codec = {
		.name = "amr-wb",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.format = "slin16",
	.newpvt = amrtolin_new,
	.framein = amrtolin_framein,
	.destroy = amrtolin_destroy,
	.sample = amrwb_sample,
	.desc_size = sizeof(struct amr_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES * 2,
};

static struct ast_translator lin16toamr = {
	.name = "lin16toamr",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.dst_codec = {
		.name = "amr-wb",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.format = "amrwb",
	.newpvt = lintoamr_new,
	.framein = lintoamr_framein,
	.frameout = lintoamr_frameout,
	.destroy = lintoamr_destroy,
	.sample = slin16_sample,
	.desc_size = sizeof(struct amr_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES * 2,
};


static int amr_packet_samples(struct ast_frame *frame, int wideband)
{
	const struct amr_attr *attr = ast_format_get_attribute_data(frame->subclass.format);
	struct amr_packet packet;
	if (frame->datalen <= 0 || amr_payload_parse(frame->data.ptr, frame->datalen,
		wideband, attr ? attr->octet_align : 0, &packet)) {
		return 0;
	}
	return packet.count * (wideband ? 320 : 160);
}

static int amr_samples(struct ast_frame *frame)
{
	return amr_packet_samples(frame, 0);
}

static int amrwb_samples(struct ast_frame *frame)
{
	return amr_packet_samples(frame, 1);
}

static struct ast_codec amr = {
	.name = "amr",
	.description = "AMR",
	.type = AST_MEDIA_TYPE_AUDIO,
	.sample_rate = 8000,
	.minimum_ms = 20,
	.maximum_ms = 20,
	.default_ms = 20,
	.samples_count = amr_samples,
};

static struct ast_codec amrwb = {
	.name = "amr-wb",
	.description = "AMR-WB",
	.type = AST_MEDIA_TYPE_AUDIO,
	.sample_rate = 16000,
	.minimum_ms = 20,
	.maximum_ms = 20,
	.default_ms = 20,
	.samples_count = amrwb_samples,
};

static struct ast_format *register_format(struct ast_codec *definition, const char *name)
{
    struct ast_codec *codec;
    struct ast_format *format;
    if (ast_codec_register(definition)) return NULL;
    codec = ast_codec_get(definition->name, definition->type, definition->sample_rate);
    if (!codec) return NULL;
    format = ast_format_create_named(name, codec);
    ao2_cleanup(codec);
    if (!format) return NULL;
    if (ast_format_cache_set(format) || ast_rtp_engine_load_format(format)) {
        ao2_cleanup(format);
        return NULL;
    }
    return format;
}

static int load_module(void)
{
    const char *names[] = { "amr", "amrwb", "amr-wb" };
    unsigned int i;
    for (i = 0; i < ARRAY_LEN(names); ++i) {
        struct ast_format *format = ast_format_cache_get(names[i]);
        struct ast_codec *codec = ast_codec_get(names[i], AST_MEDIA_TYPE_AUDIO, i ? 16000 : 8000);
        int exists = format || codec;
        ao2_cleanup(format);
        ao2_cleanup(codec);
        if (exists) {
            ast_log(LOG_ERROR, "AMR is already registered; do not combine this addon with patched AMR cores/modules\n");
            return AST_MODULE_LOAD_DECLINE;
        }
    }
    /* Asterisk cannot unregister codecs or format interfaces. After this point
     * a declined module could be dlclosed with callbacks still registered.
     * Keep a failed initialization resident, with parsing disabled. The required
     * PJSIP module checks readiness and fails startup instead of using it. */
    ast_module_shutdown_ref(AST_MODULE_SELF);
    if (amr_register_format_interfaces()) goto failed;
    addon_amr = register_format(&amr, "amr");
    addon_amrwb = register_format(&amrwb, "amrwb");
    if (!addon_amr || !addon_amrwb) goto failed;
#ifdef AMR_TEST_FAIL_AFTER_REGISTER
    goto failed;
#endif
    if (ast_register_translator(&amrtolin) || ast_register_translator(&lintoamr)
        || ast_register_translator(&amrtolin16) || ast_register_translator(&lin16toamr))
        goto failed;
    amr_formats_ready(1);
    return AST_MODULE_LOAD_SUCCESS;
failed:
    ast_log(LOG_ERROR, "AMR initialization failed; codec module retained to protect registered callbacks. Restart Asterisk after correcting the error.\n");
    return AST_MODULE_LOAD_SUCCESS;
}

/* Codec registration pins this module until process shutdown. */
static int unload_module(void) { return -1; }

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "AMR and AMR-WB codecs",
    .support_level = AST_MODULE_SUPPORT_EXTENDED,
    .load = load_module,
    .unload = unload_module,
    .load_pri = AST_MODPRI_CHANNEL_DEPEND - 10,
);
