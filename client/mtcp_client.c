/***
    
***/


#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <stdbool.h>
#include "mtcp_client.h"
#include "mtcp_common.h"

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define APP_THREAD_COLOR ANSI_COLOR_YELLOW
#define SEND_THREAD_COLOR ANSI_COLOR_BLUE
#define RECV_THREAD_COLOR ANSI_COLOR_MAGENTA

#define SYN             0x00
#define SYN_ACK         0x01
#define FIN             0x02
#define FIN_ACK         0x03
#define ACK             0x04
#define DATA            0x05
#define INITIAL_MODE    0x06

unsigned char global_send_buf[MAX_BUF_SIZE];
unsigned char global_recv_buf[MAX_BUF_SIZE];

/* -------------------- Global Variables -------------------- */

/* ThreadID for Sending Thread and Receiving Thread */
static pthread_t send_thread_pid;
static pthread_t recv_thread_pid;

static pthread_cond_t app_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t app_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t send_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t global_send_buf_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_info {
    int socket_fd;
    struct sockaddr_in* server_addr;
};

typedef enum{
    INITIAL_STATE,
    THREE_WAY_HANDSHAKE_STATE,
    DATA_TRANSMISSION_STATE,
    FOUR_WAY_HANDSHAKE_STATE
} STATE;

static int last_seq = 0;
static int cur_seq = 0; 
static int next_seq = 0;
static char last_packet_recv = INITIAL_MODE;
static STATE cur_state = INITIAL_STATE;
static int client_buf_size;
static bool conn_status = false;

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

void empty_mtcp_header(struct mtcp_header* header) {
    memset(header, 0, sizeof(struct mtcp_header));
}

void put_data(struct mtcp_header* header, unsigned char* data_, unsigned int size) {
    memset(header->data, 0, SEGMENT_SIZE);
    memcpy(header->data, data_, size);
}

void printf_helper_app(const char* state, const char* message) {
    printf("%s[%s][%s]: %s\n%s", APP_THREAD_COLOR, "Application Thread", state, message, ANSI_COLOR_RESET);
}

void printf_helper_send_no_seq(const STATE state, const char* message) {
    char *state_msg;
    switch (state) {
        case INITIAL_STATE:
            state_msg = "INITIAL STATE";
        break;
        case THREE_WAY_HANDSHAKE_STATE:
            state_msg = "THREE_WAY_HANDSHAKE_STATE";
        break;
        case DATA_TRANSMISSION_STATE:
            state_msg = "DATA_TRANSMISSION_STATE";
        break;
        case FOUR_WAY_HANDSHAKE_STATE:
            state_msg = "FOUR_WAY_HANDSHAKE_STATE";
        break;
    }
    printf("%s[%s][%s]: %s\n%s", SEND_THREAD_COLOR, "Sending Thread", state_msg, message, ANSI_COLOR_RESET);
}

void printf_helper_send_with_seq(const STATE state, const char* message, int seq, char* mode) {
    char *state_msg;
    switch (state) {
        case INITIAL_STATE:
            state_msg = "INITIAL STATE";
        break;
        case THREE_WAY_HANDSHAKE_STATE:
            state_msg = "THREE_WAY_HANDSHAKE_STATE";
        break;
        case DATA_TRANSMISSION_STATE:
            state_msg = "DATA_TRANSMISSION_STATE";
        break;
        case FOUR_WAY_HANDSHAKE_STATE:
            state_msg = "FOUR_WAY_HANDSHAKE_STATE";
        break;
    }
    printf("%s[%s][%s]: %s [sent seq:%d][sent mode:%s]\n%s", SEND_THREAD_COLOR, "Sending Thread", state_msg, message, seq, mode, ANSI_COLOR_RESET);
}

void printf_helper_recv_no_seq(const STATE state, const char* message) {
    char *state_msg;
    switch (state) {
        case INITIAL_STATE:
            state_msg = "INITIAL STATE";
        break;
        case THREE_WAY_HANDSHAKE_STATE:
            state_msg = "THREE_WAY_HANDSHAKE_STATE";
        break;
        case DATA_TRANSMISSION_STATE:
            state_msg = "DATA_TRANSMISSION_STATE";
        break;
        case FOUR_WAY_HANDSHAKE_STATE:
            state_msg = "FOUR_WAY_HANDSHAKE_STATE";
        break;
    }
    printf("%s[%s][%s]: %s\n%s", RECV_THREAD_COLOR, "Receving Thread", state_msg, message, ANSI_COLOR_RESET);
}

void printf_helper_recv_with_seq(const STATE state, const char* message, int seq, char* mode) {
    char *state_msg;
    switch (state) {
        case INITIAL_STATE:
            state_msg = "INITIAL STATE";
        break;
        case THREE_WAY_HANDSHAKE_STATE:
            state_msg = "THREE_WAY_HANDSHAKE_STATE";
        break;
        case DATA_TRANSMISSION_STATE:
            state_msg = "DATA_TRANSMISSION_STATE";
        break;
        case FOUR_WAY_HANDSHAKE_STATE:
            state_msg = "FOUR_WAY_HANDSHAKE_STATE";
        break;
    }
    printf("%s[%s][%s]: %s [recv ack:%d][recv mode:%s]\n%s", RECV_THREAD_COLOR, "Receiving Thread", state_msg, message, seq, mode, ANSI_COLOR_RESET);
}

static void *send_thread(void *args){
    int socket_fd = ((struct thread_info*)args)->socket_fd;
    struct sockaddr_in *server_addr = ((struct thread_info*)args)->server_addr;
    unsigned int addrlen = sizeof(struct sockaddr_in);

    int len; // variable used to monitor the number of bytes sent
    struct mtcp_header header;

    unsigned char packet_to_send; // option sent to server side
    unsigned int read_last_seq; 
    unsigned int read_cur_seq;
    // unsigned int read_next_seq; 
    STATE read_state; // local version of global cur_state
    char read_last_packet_recv; // local version of last_packet_recv

    struct timespec timeToWait;
    struct timeval now;

    bool connection_in_use = true;
    while(connection_in_use) {
        gettimeofday(&now, NULL);
        timeToWait.tv_sec = now.tv_sec + 1;

        pthread_mutex_lock(&info_mutex);
        read_state = cur_state;
        pthread_mutex_unlock(&info_mutex);

        if (read_state == DATA_TRANSMISSION_STATE) {
            pthread_mutex_lock(&send_thread_sig_mutex);
            pthread_cond_timedwait(&send_thread_sig, &send_thread_sig_mutex, &timeToWait);
            pthread_mutex_unlock(&send_thread_sig_mutex);
        }
        else {
            printf("Sending: I'M WAITING \n");
            pthread_mutex_lock(&send_thread_sig_mutex);
            pthread_cond_wait(&send_thread_sig, &send_thread_sig_mutex);
            pthread_mutex_unlock(&send_thread_sig_mutex);
        }

        pthread_mutex_lock(&info_mutex);
        read_state = cur_state;
        read_last_packet_recv = last_packet_recv;
        read_cur_seq = cur_seq;
        pthread_mutex_unlock(&info_mutex);

        switch(read_state) {
            case THREE_WAY_HANDSHAKE_STATE:
                switch (read_last_packet_recv) {
                    case INITIAL_MODE:
                        packet_to_send = SYN;
                        empty_mtcp_header(&header);
                        encode_mtcp_header(&header, packet_to_send, read_cur_seq);
                        if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr *)server_addr, addrlen)) != 4) {
                            perror("Sending thread: Client fails to send SYN to server");
                            exit(-1);
                        }
                        pthread_mutex_lock(&info_mutex);
                        next_seq += 1;
                        pthread_mutex_unlock(&info_mutex);
                        printf_helper_send_with_seq(read_state, "sent packet", read_cur_seq, "SYN");
                    break;
                    case SYN_ACK:
                        packet_to_send = ACK;
                        empty_mtcp_header(&header);
                        encode_mtcp_header(&header, packet_to_send, read_cur_seq);
                        if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr *)server_addr, addrlen)) != 4) {
                            perror("Sending thread: Client fails to send ACK to server");
                        }
                        pthread_mutex_lock(&info_mutex);
                        next_seq += 1;
                        last_seq = cur_seq;
                        conn_status = true;
                        // cur_state = DATA_TRANSMISSION_STATE;
                        pthread_mutex_unlock(&info_mutex);
                        printf_helper_send_with_seq(read_state, "sent packet", read_cur_seq, "ACK");
                        pthread_mutex_lock(&app_thread_sig_mutex);
                        pthread_cond_signal(&app_thread_sig);
                        pthread_mutex_unlock(&app_thread_sig_mutex);

                    break;
                    default:
                        perror("Sending Thread: expects last packet received in THREE_WAY_HANDSHAKE_STATE to be INITIAL or SYN_ACK\n");
                        exit(-1);
                    break;
                }
            break;

            case DATA_TRANSMISSION_STATE:
                pthread_mutex_lock(&info_mutex);
                read_last_seq = last_seq;
                read_cur_seq = cur_seq;
                pthread_mutex_unlock(&info_mutex);
                // timeout! retransmit
                if (read_last_seq == read_cur_seq) {
                    if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr *)server_addr, addrlen)) != 4) {
                        perror("Sending thread: Client fails to send ACK to server");
                    }
                } 
                // woken up
                // check whether global_send_buf is empty
                // if yes -> block, waiting for app thread to wake up
                // if no -> read data, prepare new packet and send  
                else {
                    pthread_mutex_lock(&info_mutex);
                    len = client_buf_size;
                    pthread_mutex_unlock(&info_mutex);   
                    if (len != 0) {
                        empty_mtcp_header(&header);
                        pthread_mutex_lock(&global_send_buf_mutex);
                        put_data(&header, global_send_buf, len);
                        memset(&global_send_buf, 0, MAX_BUF_SIZE);
                        pthread_mutex_unlock(&global_send_buf_mutex); 
                        packet_to_send = DATA;
                        encode_mtcp_header(&header, packet_to_send, read_cur_seq);
                        if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr *)server_addr, addrlen)) != 4) {
                            perror("Sending thread: Client fails to send DATA to server");
                        }
                        pthread_mutex_lock(&info_mutex);
                        last_seq = cur_seq;
                        next_seq = cur_seq + len;
                        client_buf_size = 0;
                        pthread_mutex_unlock(&info_mutex); 
                        printf_helper_send_with_seq(read_state, "sent packet", read_cur_seq, "DATA");
                    }
                }
            break;

            case FOUR_WAY_HANDSHAKE_STATE:
                switch(read_last_packet_recv) {
                    // This ACK is from the last state (Data transmission state), hence send FIN
                    case ACK:
                        packet_to_send = FIN;

                        pthread_mutex_lock(&info_mutex);
                        len = client_buf_size;
                        pthread_mutex_unlock(&info_mutex);   
                        if (len != 0) {
                            empty_mtcp_header(&header);
                            pthread_mutex_lock(&global_send_buf_mutex);
                            put_data(&header, global_send_buf, len);
                            memset(&global_send_buf, 0, MAX_BUF_SIZE);
                            pthread_mutex_unlock(&global_send_buf_mutex); 
                            encode_mtcp_header(&header, packet_to_send, read_cur_seq);
                            if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr *)server_addr, addrlen)) != 4) {
                                perror("Sending thread: Client fails to send DATA to server");
                            }
                            pthread_mutex_lock(&info_mutex);
                            last_seq = cur_seq;
                            next_seq = cur_seq + len;
                            client_buf_size = 0;
                            pthread_mutex_unlock(&info_mutex); 
                            printf_helper_send_with_seq(read_state, "sent packet to client", read_cur_seq, "FIN");
                        } else {
                            empty_mtcp_header(&header);
                            encode_mtcp_header(&header, packet_to_send, read_cur_seq);
                            if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr *)server_addr, addrlen)) != 4) {
                                perror("Sending thread: Client fails to send SYN to server");
                                exit(-1);
                            }
                            printf_helper_send_with_seq(read_state, "sent packet to client", read_cur_seq, "FIN");
                        }
                    break;

                    case FIN_ACK:
                        packet_to_send = ACK;
                        empty_mtcp_header(&header);
                        encode_mtcp_header(&header, packet_to_send, read_cur_seq);
                        if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr *)server_addr, addrlen)) != 4) {
                            perror("Sending thread: Client fails to send ACK to server");
                        }
                        pthread_mutex_lock(&info_mutex);
                        next_seq += 1;
                        last_seq = cur_seq;
                        conn_status = false;
                        pthread_mutex_unlock(&info_mutex);
                        printf_helper_send_with_seq(read_state, "sent packet", read_cur_seq, "ACK");
                        pthread_mutex_lock(&app_thread_sig_mutex);
                        pthread_cond_signal(&app_thread_sig);
                        pthread_mutex_unlock(&app_thread_sig_mutex);
                        connection_in_use = false;
                    break;

                    default:
                        perror("Sending Thread: expects last packet received in FOUR_WAY_HANDSHAKE_STATE to be ACK or FIN_ACK\n");
                        exit(-1);
                    break;
                }
            break;

            default:
                perror("Fatal: Sending thread sees INITIAL_STATE");
            break;
        }
    }
}

static void *receive_thread(void *args){
    int len; // variable used to monitor the number of bytes received
    unsigned int ack_recv; // variable used to compare against seq_to_send
    // unsigned int read_cur_seq; // local version of seq_to_send
    unsigned int read_next_seq; // local version of last_seq_to_send
    unsigned char packet_recv;  // variable used to change global last_packet_recv
    STATE read_state; // local version of cur_state

    int socket_fd = ((struct thread_info*)args)->socket_fd;
    struct mtcp_header header;

    bool monitoring = true;
    while(monitoring) {
        if ((len = recvfrom(socket_fd, (char*)&header, 4, 0, NULL, NULL)) < 4) {
            perror("Server receives incorrect data from server\n");
            exit(-1);
        }
        decode_mtcp_header(&header, &packet_recv, &ack_recv);
        switch(packet_recv) {
            // Only THREE WAY HANDSHAKE has SYN_ACK
            case SYN_ACK:
                pthread_mutex_lock(&info_mutex);
                last_packet_recv = packet_recv;
                read_state = cur_state;
                cur_seq = next_seq;
                // change value of sequence number so that sending thread can send new one
                // seq_to_send = ack_recv;
                // last_seq_sent = 0;
                pthread_mutex_unlock(&info_mutex);
                printf_helper_recv_with_seq(read_state, "wake sending thread", ack_recv, "SYN_ACK");
                pthread_mutex_lock(&send_thread_sig_mutex);
                pthread_cond_signal(&send_thread_sig);
                pthread_mutex_unlock(&send_thread_sig_mutex);
            break;
            // Only FOUR WAY HANDSHAKE has FIN_ACK
            case FIN_ACK:
                pthread_mutex_lock(&info_mutex);
                last_packet_recv = packet_recv;
                read_state = cur_state;
                cur_seq = next_seq;
                // change value of sequence number so that sending thread can send new one
                // seq_to_send = ack_recv;

                pthread_mutex_unlock(&info_mutex);
                printf_helper_recv_with_seq(read_state, "wake sending thread", ack_recv, "FIN_ACK");
                pthread_mutex_lock(&send_thread_sig_mutex);
                pthread_cond_signal(&send_thread_sig);
                pthread_mutex_unlock(&send_thread_sig_mutex);
            break;
            // Only DATA TRANSMISSION has ACK
            case ACK:
                pthread_mutex_lock(&info_mutex);
                read_next_seq = next_seq;
                pthread_mutex_unlock(&info_mutex);
                if (read_next_seq == ack_recv) {
                    pthread_mutex_lock(&info_mutex);
                    cur_seq = next_seq;
                    pthread_mutex_unlock(&info_mutex);                    
                    pthread_mutex_lock(&send_thread_sig_mutex);
                    pthread_cond_signal(&send_thread_sig);
                    pthread_mutex_unlock(&send_thread_sig_mutex);
                }
            break;

            default:
                perror("Server receives incorrect mode from server\n");
                exit(-1);
            break;
        }
    }
}

/* Connect Function Call (mtcp Version) */
void mtcp_connect(int socket_fd, struct sockaddr_in *server_addr){
    STATE local_state = INITIAL_STATE;
    struct thread_info args;
    args.socket_fd = socket_fd;
    args.server_addr = server_addr;

    pthread_mutex_lock(&info_mutex);
    cur_state = THREE_WAY_HANDSHAKE_STATE;
    pthread_mutex_unlock(&info_mutex);

    if (pthread_create(&send_thread_pid, NULL, &send_thread, &args)) {
        perror("Fail to create sending thread");
    }
    if (pthread_create(&recv_thread_pid, NULL, &receive_thread, &args)) {
        perror("Fail to create receving thread");
    }
    sleep(1);

    printf_helper_app("THREE_WAY_HANDSHAKE_STATE", "wake sending thread to initiate THREE_WAY_HANDSHAKE");
    pthread_mutex_lock(&send_thread_sig_mutex);
    pthread_cond_signal(&send_thread_sig);
    pthread_mutex_unlock(&send_thread_sig_mutex);

    pthread_mutex_lock(&info_mutex);
    local_state = cur_state;
    pthread_mutex_unlock(&info_mutex);

    if (local_state == THREE_WAY_HANDSHAKE_STATE) {
        printf_helper_app("THREE_WAY_HANDSHAKE_STATE", "sleep after waking sending thread up");
        pthread_mutex_lock(&app_thread_sig_mutex);
        pthread_cond_wait(&app_thread_sig, &app_thread_sig_mutex);
        pthread_mutex_unlock(&app_thread_sig_mutex);
        printf_helper_app("THREE_WAY_HANDSHAKE_STATE", "THREE WAY HANDSHAKE complete and mtcp connect return");
    }
    return;
}

/* Write Function Call (mtcp Version) */
int mtcp_write(int socket_fd, unsigned char *buf, int buf_len){
    // write buf to global_send_buf
    // wake sending thread up and return buf_len automatically
    // ????? size of global_send_buf is only 1004
    // ????? how can mtcp_write return automatically and still ensure that every packet will be delivered
    bool local_conn_status;
    STATE local_state;

    pthread_mutex_lock(&info_mutex);
    local_state = cur_state;
    local_conn_status = conn_status;
    pthread_mutex_unlock(&info_mutex);

    if (!local_conn_status) {
        return -1;
    }
    if (local_state == FOUR_WAY_HANDSHAKE_STATE) {
        return 0;
    }
    pthread_mutex_lock(&global_send_buf_mutex);
    memcpy(&global_send_buf, buf, buf_len);
    pthread_mutex_unlock(&global_send_buf_mutex);

    pthread_mutex_lock(&info_mutex);
    cur_state = DATA_TRANSMISSION_STATE;
    pthread_mutex_unlock(&info_mutex);

    pthread_mutex_lock(&send_thread_sig_mutex);
    pthread_cond_signal(&send_thread_sig);
    pthread_mutex_unlock(&send_thread_sig_mutex);

    return buf_len;
}

/* Close Function Call (mtcp Version) */
void mtcp_close(int socket_fd){
    bool local_conn_status;
    pthread_mutex_lock(&info_mutex);
    cur_state = FOUR_WAY_HANDSHAKE_STATE;
    local_conn_status = conn_status;
    pthread_mutex_unlock(&info_mutex);
    if (!local_conn_status) {
        return;
    }

    printf_helper_app("FOUR_WAY_HANDSHAKE_STATE", "wake up sending thread to initiate FOUR_WAY_HANDSHAKE");
    pthread_mutex_lock(&send_thread_sig_mutex);
    pthread_cond_signal(&send_thread_sig);
    pthread_mutex_unlock(&send_thread_sig_mutex);

    pthread_mutex_lock(&app_thread_sig_mutex);
    pthread_cond_wait(&app_thread_sig, &app_thread_sig_mutex);
    pthread_mutex_unlock(&app_thread_sig_mutex);
    printf_helper_app("FOUR_WAY_HANDSHAKE_STATE", "FOUR_WAY_HANDSHAKE complete and mtcp_close return!");
    
    return;
}
