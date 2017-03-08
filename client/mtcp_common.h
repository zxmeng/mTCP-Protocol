#include "stdint.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define MAX_BUF_SIZE 1024
#define SERVER_PORT	12345
#define SEGMENT_SIZE 1000

struct m_tcp_header {
    uint32_t type_seq;
    char data[SEGMENT_SIZE];
};

uint32_t get_type(struct m_tcp_header* header) {
    return header->type_seq >> 28;
}

uint32_t get_seq_ack(struct m_tcp_header* header) {
    return header->type_seq << 4 >> 4;
}

void put_type(struct m_tcp_header* header, uint32_t type) {
    header->type_seq = (header->type_seq << 4 >> 4) + (type << 28);
}

void put_seq_ack(struct m_tcp_header* header, uint32_t seq_ack) {
    header->type_seq = (header->type_seq >> 28 << 28) + (seq_ack << 4 >> 4);
}

void put_data(struct m_tcp_header* header, char* data_, uint32_t size) {
    memset(header->data, 0, SEGMENT_SIZE);
    memcpy(header->data, data_, size);
}

