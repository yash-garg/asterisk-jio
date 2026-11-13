#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#if __has_include("amr_payload.h")
#include "amr_payload.h"
int main(void)
{
    struct amr_packet p;
    unsigned char mime[64], wire[64];
    /* AMR 4.75: 95 speech bits; BE has 4 CMR + 6 ToC bits. */
    const unsigned char be[] = {0xf0,0x6d,0x47,0x8c,0xc3,0x0d,0x03,0xec,0xe2,0x18,0x3e,0x28,0x20,0x80};
    const unsigned char storage[] = {0x04,0xb5,0x1e,0x33,0x0c,0x34,0x0f,0xb3,0x88,0x60,0xf8,0xa0,0x82};
    assert(amr_payload_parse(be,sizeof(be),0,0,&p)==0 && p.count==1 && p.cmr==15);
    assert(amr_payload_frame(be,&p,0,mime)==13 && !memcmp(mime,storage,13));
    assert(amr_payload_pack(storage,13,0,0,wire,sizeof(wire))==14 && !memcmp(wire,be,14));
    assert(amr_payload_pack(storage,13,0,1,wire,sizeof(wire))==14);
    assert(wire[0]==0xf0 && !memcmp(wire+1,storage,13));
    assert(amr_payload_parse(wire,14,0,1,&p)==0 && p.count==1);
    /* RFC 4867 section 4.4.5.1: two octet-aligned AMR 7.95 frames. */
    unsigned char multi[43]={0x60,0xac,0x2c};
    assert(amr_payload_parse(multi,43,0,1,&p)==0 && p.count==2 && p.cmr==6);
    assert(amr_payload_frame(multi,&p,1,mime)==21 && mime[0]==0x2c);
    for (size_t i=0; i<sizeof(be); ++i) assert(amr_payload_parse(be,i,0,0,&p)<0);
    assert(amr_payload_parse(multi,42,0,1,&p)<0);
    assert(amr_payload_parse(multi,44,0,1,&p)<0);
    unsigned char bad[]={0xf0,0x4c}; /* NB reserved FT9 */
    assert(amr_payload_parse(bad,2,0,1,&p)<0);
    unsigned char lost[]={0xf0,0x74}; /* WB lost FT14 */
    assert(amr_payload_parse(lost,2,1,1,&p)==0 && p.count==1);
    assert(amr_payload_parse(lost,2,0,1,&p)<0);
    unsigned char wb[62]={0xf0,0x44}; /* WB 23.85, 477 bits */
    assert(amr_payload_parse(wb,62,1,1,&p)==0 && p.count==1);
    assert(amr_payload_parse(wb,61,1,1,&p)<0);
    unsigned char toc[52]; memset(toc,0xfc,sizeof(toc)); toc[0]=0xf0;
    assert(amr_payload_parse(toc,sizeof(toc),1,1,&p)<0);
    assert(amr_payload_pack(storage,12,0,0,wire,sizeof(wire))<0);
    assert(amr_payload_pack(storage,13,0,0,wire,13)<0);
    puts("PASS: RFC payload fixtures, multiple frames, both alignments, truncation and reserved modes");
}
#else
int main(void) { fputs("FAIL: bounded AMR RTP depacketizer missing\n",stderr); return 1; }
#endif
