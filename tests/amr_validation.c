#include "asterisk.h"
#include <math.h>
#include "asterisk/module.h"
#include "asterisk/format.h"
#include "asterisk/format_cache.h"
#include "asterisk/codec.h"
#include "asterisk/frame.h"
#include "asterisk/translate.h"
#include "asterisk/linkedlists.h"
#include "asterisk/strings.h"
#include "asterisk/res_pjsip.h"
#include <pjmedia/sdp_neg.h>
#include "amr_payload.h"
#include <opencore-amrnb/interf_dec.h>
#include <opencore-amrwb/dec_if.h>

static struct ast_format *ast_format_amr, *ast_format_amrwb;

#define CHECK(x) do { if (!(x)) { ast_log(LOG_ERROR, "AMR VALIDATION FAIL line %d: %s\n", __LINE__, #x); return -1; } } while (0)

static int format_tests(void)
{
    const char *bad[] = {"octet-align=2", "crc=1", "robust-sorting=1", "interleaving=0", "mode-set=8", "mode-set=", "mode-set=0,", "mode-set=-1", "mode-set=1x", "mode-change-period=0", "mode-change-period=3", "mode-change-capability=0", "channels=2", "octet-align=0;octet-align=1", "max-red=999999999999999999999999"};
    unsigned int i;
    for (i=0;i<sizeof(bad)/sizeof(*bad);++i) CHECK(ast_format_parse_sdp_fmtp(ast_format_amr,bad[i])==NULL);
    struct ast_format *a=ast_format_parse_sdp_fmtp(ast_format_amr,"octet-align=1;mode-set=0,2,7;mode-change-period=2;mode-change-neighbor=1");
    struct ast_format *b=ast_format_parse_sdp_fmtp(ast_format_amr,"octet-align=0;mode-set=0,2,7");
    struct ast_format *c=ast_format_parse_sdp_fmtp(ast_format_amr,"octet-align=1;mode-set=0,2");
    CHECK(a && b && c);
    CHECK(ast_format_joint(a,b)==NULL);
    CHECK(ast_format_joint(a,c)==NULL);
    struct ast_str *before=ast_str_create(128), *after=ast_str_create(128), *s=ast_str_create(128);
    ast_format_generate_sdp_fmtp(a,97,&before);
    struct ast_format *joint=ast_format_joint(a,ast_format_amr);
    CHECK(joint && joint != a);
    ast_format_generate_sdp_fmtp(a,97,&after);
    ast_format_generate_sdp_fmtp(joint,97,&s);
    CHECK(!strcmp(ast_str_buffer(before),ast_str_buffer(after)));
    CHECK(!strcmp(ast_str_buffer(before),ast_str_buffer(s)));
    CHECK(strstr(ast_str_buffer(s),"octet-align=1"));
    CHECK(strstr(ast_str_buffer(s),"mode-set=0,2,7"));
    CHECK(strstr(ast_str_buffer(s),"mode-change-capability=2"));
    ao2_cleanup(a); ao2_cleanup(b); ao2_cleanup(c); ao2_cleanup(joint);
    ast_free(before); ast_free(after); ast_free(s);
    ast_log(LOG_NOTICE,"AMR VALIDATION PASS: SDP validation, compatibility, mode-set preservation and immutable inputs\n");
    return 0;
}

static int codec_test(int wb,int aligned,int mode)
{
    int n=wb?320:160, count=0;
    int16_t pcm[1280], independent[320];
    char fmtp[80];
    snprintf(fmtp,sizeof(fmtp),"octet-align=%d;mode-set=%d",aligned,mode);
    struct ast_format *format=ast_format_parse_sdp_fmtp(wb?ast_format_amrwb:ast_format_amr,fmtp);
    struct ast_format *linear=wb?ast_format_slin16:ast_format_slin;
    struct ast_trans_pvt *enc=ast_translator_build_path(format,linear);
    struct ast_trans_pvt *dec=ast_translator_build_path(linear,format);
    struct ast_trans_pvt *g711=ast_translator_build_path(format,ast_format_ulaw);
    struct ast_trans_pvt *g711back=ast_translator_build_path(ast_format_alaw,format);
    void *decoder=wb?D_IF_init():Decoder_Interface_init();
    CHECK(format && enc && dec && decoder && g711 && g711back);
    for(int i=0;i<n*4;++i) pcm[i]=(int16_t)(9000*sin(i*2*3.141592653589793*440/(wb?16000:8000)));
    struct ast_frame input={.frametype=AST_FRAME_VOICE,.subclass.format=linear,.data.ptr=pcm,.samples=n*4,.datalen=n*4*2};
    struct ast_frame *frames=ast_translate(enc,&input,0),*f;
    CHECK(frames);
    double energy=0;
    for(f=frames;f;f=AST_LIST_NEXT(f,frame_list)) {
        unsigned char storage[AMR_STORAGE_BYTES]; struct amr_packet packet;
        CHECK(f->samples==n && ast_codec_samples_count(f)==(unsigned int)n);
        CHECK(amr_payload_parse(f->data.ptr,f->datalen,wb,aligned,&packet)==0 && packet.count==1);
        amr_payload_frame(f->data.ptr,&packet,0,storage);
        CHECK(((storage[0]>>3)&15)==mode);
        if(wb) D_IF_decode(decoder,storage,independent,0); else Decoder_Interface_Decode(decoder,storage,independent,0);
        struct ast_frame one=*f;
        AST_LIST_NEXT(&one,frame_list)=NULL;
        struct ast_frame *decoded=ast_translate(dec,&one,0);
        CHECK(decoded && decoded->samples==n && decoded->datalen==n*2);
        CHECK(!memcmp(decoded->data.ptr,independent,n*2));
        int16_t *samples=decoded->data.ptr;
        for(int i=0;i<n;++i) energy+=(double)samples[i]*samples[i];
        ast_frfree(decoded); ++count;
    }
    CHECK(count==4 && energy>1000000);
    struct ast_frame *batch=ast_translate(dec,frames,0);
    CHECK(batch && batch->samples==n*4 && batch->datalen==n*8);
    ast_frfree(batch);
    /* Two received octet-aligned frames must decode to 40 ms, not 20 ms. */
    if(aligned) {
        unsigned char packet[130]={0xf0};
        struct ast_frame *second=AST_LIST_NEXT(frames,frame_list);
        size_t speech=frames->datalen-2;
        packet[1]=((unsigned char *)frames->data.ptr)[1]|0x80;
        packet[2]=((unsigned char *)second->data.ptr)[1];
        memcpy(packet+3,(unsigned char *)frames->data.ptr+2,speech);
        memcpy(packet+3+speech,(unsigned char *)second->data.ptr+2,speech);
        struct ast_frame multi={.frametype=AST_FRAME_VOICE,.subclass.format=format,.data.ptr=packet,.samples=n*2,.datalen=3+speech*2};
        CHECK(ast_codec_samples_count(&multi)==(unsigned int)n*2);
        struct ast_frame *decoded=ast_translate(dec,&multi,0);
        CHECK(decoded && decoded->samples==n*2 && decoded->datalen==n*4);
        ast_frfree(decoded);
        multi.datalen--;
        CHECK(ast_codec_samples_count(&multi)==0 && ast_translate(dec,&multi,0)==NULL);
    }
    ast_frfree(frames);
    ast_translator_free_path(enc); ast_translator_free_path(dec);
    ast_translator_free_path(g711); ast_translator_free_path(g711back);
    if(wb) D_IF_exit(decoder); else Decoder_Interface_exit(decoder);
    ao2_cleanup(format);
    ast_log(LOG_NOTICE,"AMR VALIDATION PASS: %s aligned=%d mode=%d: four audio frames, independent decode and G.711 paths\n",wb?"WB":"NB",aligned,mode);
    return 0;
}
static int offer_answer_tests(void)
{
    const char *offers[] = {
        "octet-align=0;mode-set=0,2", "octet-align=0;mode-set=0,2",
        "octet-align=0;mode-set=0,2", "octet-align=0",
    };
    const char *answers[] = {
        "octet-align=0;mode-set=0,2", "octet-align=0;mode-set=0",
        "octet-align=0", "octet-align=0;mode-set=0,2",
    };
    const int accepted[] = { 1, 0, 0, 1 };
    for (unsigned int i = 0; i < ARRAY_LEN(offers); ++i) {
        pj_pool_t *pool = pjsip_endpt_create_pool(ast_sip_get_pjsip_endpoint(), "amr-test", 4096, 4096);
        pjmedia_sdp_session *offer, *answer;
        char obuf[512], abuf[512];
        const char *base = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 40000 RTP/AVP 97\r\na=rtpmap:97 AMR/8000\r\na=fmtp:97 %s\r\n";
        CHECK(pool);
        snprintf(obuf, sizeof(obuf), base, offers[i]);
        snprintf(abuf, sizeof(abuf), base, answers[i]);
        CHECK(pjmedia_sdp_parse(pool, obuf, strlen(obuf), &offer) == PJ_SUCCESS);
        CHECK(pjmedia_sdp_parse(pool, abuf, strlen(abuf), &answer) == PJ_SUCCESS);
        pj_status_t status = pjmedia_sdp_neg_fmt_match(pool, offer->media[0], 0, answer->media[0], 0, 0);
        pj_pool_release(pool);
        CHECK((status == PJ_SUCCESS) == accepted[i]);
    }
    ast_log(LOG_NOTICE, "AMR VALIDATION PASS: PJPROJECT preserves explicit offered mode sets\n");
    return 0;
}

static int load_module(void)
{
    ast_format_amr=ast_format_cache_get("amr");
    ast_format_amrwb=ast_format_cache_get("amrwb");
    CHECK(ast_format_amr && ast_format_amrwb);
    if(format_tests()) return AST_MODULE_LOAD_DECLINE;
    if(offer_answer_tests()) return AST_MODULE_LOAD_DECLINE;
    for(int wb=0;wb<=1;++wb) for(int aligned=0;aligned<=1;++aligned) for(int mode=0;mode<=(wb?8:7);++mode)
        if(codec_test(wb,aligned,mode)) return AST_MODULE_LOAD_DECLINE;
    ast_log(LOG_NOTICE,"AMR VALIDATION ALL PASSED\n");
    return AST_MODULE_LOAD_SUCCESS;
}
static int unload_module(void) { ao2_cleanup(ast_format_amr); ao2_cleanup(ast_format_amrwb); return 0; }
AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY,"Temporary AMR offline validation");
