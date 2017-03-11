#include "stdint.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define MAX_BUF_SIZE 1024
#define SERVER_PORT	12345
#define SEGMENT_SIZE 1000

#define SYN     0x00
#define SYN_ACK 0x01
#define FIN     0x02
#define FIN_ACK 0x03
#define ACK     0x04
#define DATA    0x05

struct mtcp_header {
    unsigned char header_[4];
    unsigned char data[SEGMENT_SIZE];
};

void encode_mtcp_header(struct mtcp_header* header, unsigned char mode, unsigned int seq_ack) {
    seq_ack = htonl(seq_ack);
    memcpy(header->header_, &seq_ack, 4);
    header->header_[0] = header->header_[0] | (mode << 4);
}

void decode_mtcp_header(struct mtcp_header* header, unsigned char* mode, unsigned int* seq_ack) {
    *mode = header->header_[0] >> 4;
    // mask out the first 4 bit
    header->header_[0] = header->header_[0] & 0x0F;
    memcpy(seq_ack, header->header_, 4);
    *seq_ack = ntohl(*seq_ack);
}

void put_data(struct mtcp_header* header, char* data_, unsigned int size) {
    memset(header->data, 0, SEGMENT_SIZE);
    memcpy(header->data, data_, size);
}
