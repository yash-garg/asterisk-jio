#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "amr_payload.h"
int main(void) {
    unsigned char data[3200], frame[AMR_STORAGE_BYTES];
    struct amr_packet packet;
    uint32_t rng=37;
    for (int j=0;j<100000;++j) {
        size_t len=(rng>>16)%sizeof(data);
        for(size_t i=0;i<len;++i) { rng=1664525*rng+1013904223; data[i]=rng>>24; }
        for(int wb=0;wb<=1;++wb) for(int align=0;align<=1;++align) {
            if(!amr_payload_parse(data,len,wb,align,&packet)) {
                assert(packet.count>0 && packet.count<=50);
                for(unsigned int i=0;i<packet.count;++i) {
                    int n=amr_payload_frame(data,&packet,i,frame);
                    assert(n>0 && n<=AMR_STORAGE_BYTES);
                }
            }
        }
        rng=1664525*rng+1013904223;
    }
    return 0;
}
