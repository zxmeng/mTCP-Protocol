#include "mtcp_common.h"

int main() {
    struct m_tcp_header* header = (struct m_tcp_header*)malloc(sizeof(struct m_tcp_header));
    memset(header, 0, SEGMENT_SIZE + 4);
    put_type(header, 5);
    put_seq_ack(header, 20);
    put_data(header, "abc", 4);

    printf("type is %u\n", get_type(header));
    printf("seq/ack is %d\n", get_seq_ack(header));

    put_type(header, 6);
    put_seq_ack(header, 100000);

    printf("type is %d\n", get_type(header));
    printf("seq/ack is %d\n", get_seq_ack(header));

    return 0;
}
