#include "mtcp_common.h"

int main() {
    struct mtcp_header* header = (struct mtcp_header*)malloc(sizeof(struct mtcp_header));
    unsigned char mode;
    unsigned int seq_ack;
    memset(header, 0, SEGMENT_SIZE + 4);
    encode_mtcp_header(header, FIN, 1000);
    decode_mtcp_header(header, &mode, &seq_ack);

    switch(mode) {
        case SYN:
            printf("mode is SYN\n");
            break;
        case SYN_ACK:
            printf("mode is SYN_ACK\n");
            break;
        case FIN:
            printf("mode is FIN\n");
            break;
        case FIN_ACK:
            printf("mode is FIN_ACK\n");
            break;
        case ACK:
            printf("mode is ACK\n");
            break;
        case DATA:
            printf("mode is DATA\n");
            break;
        default:
            printf("unrecognized mode\n");
            break;
    }
    printf("seq/ack is %u\n", seq_ack);

    return 0;
}
