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
#include "mtcp_common.h"
#include "mtcp_server.h"

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define APP_THREAD_COLOR  ANSI_COLOR_YELLOW
#define SEND_THREAD_COLOR ANSI_COLOR_BLUE
#define RECV_THREAD_COLOR ANSI_COLOR_MAGENTA

#define SYN             0x00
#define SYN_ACK         0x01
#define FIN             0x02
#define FIN_ACK         0x03
#define ACK             0x04
#define DATA            0x05
#define INITIAL_MODE    0x06

#define FILE_MAX_SIZE ((1 << 28) - 3)

unsigned int min(unsigned int a, unsigned int b) {
    return a > b ? b : a;
}

typedef enum{
    INITIAL_STATE,
    THREE_WAY_HANDSHAKE_STATE,
    DATA_TRANSMISSION_STATE,
    FOUR_WAY_HANDSHAKE_STATE
} STATE;

struct mtcp_packet {
    unsigned char header_[4];
    unsigned char data[1000];
};

void encode_mtcp_header(struct mtcp_packet* packet, unsigned char type, unsigned int seq_ack) {
    seq_ack = htonl(seq_ack);
    memcpy(packet->header_, &seq_ack, 4);
    packet->header_[0] = packet->header_[0] | (type << 4);
}

void decode_mtcp_header(struct mtcp_packet* packet, unsigned char* type, unsigned int* seq_ack) {
    *type = packet->header_[0] >> 4;
    // mask out the first 4 bit
    packet->header_[0] = packet->header_[0] & 0x0F;
    memcpy(seq_ack, packet->header_, 4);
    *seq_ack = ntohl(*seq_ack);
}

unsigned char * get_data(struct mtcp_packet* packet) {
    return packet->data;
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

void printf_helper_send_with_seq(const STATE state, const char* message, int seq, const char* type) {
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
    printf("%s[%s][%s]: %s [seq:%d][type:%s]\n%s", SEND_THREAD_COLOR, "Sending Thread", state_msg, message, seq, type, ANSI_COLOR_RESET);
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

void printf_helper_recv_with_seq(const STATE state, const char* message, int seq, const char* type) {
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
    printf("%s[%s][%s]: %s [seq:%d][type:%s]\n%s", RECV_THREAD_COLOR, "Receiving Thread", state_msg, message, seq, type, ANSI_COLOR_RESET);
}

/* ThreadID for Sending Thread and Receiving Thread */
static pthread_t send_thread_pid;
static pthread_t recv_thread_pid;

static pthread_cond_t app_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t app_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t send_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t local_recv_buf_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;

static char local_recv_buf[FILE_MAX_SIZE];


struct thread_info {
    int socket_fd;
    struct sockaddr_in* server_addr;
};

struct sockaddr_in client_addr;

static STATE cur_state = INITIAL_STATE;
static unsigned int cur_ack_num = 0;
static unsigned int local_recv_buf_len = 0;
static unsigned int app_thread_read_len = 0;
static int recv_from_len = 0;


static void *send_thread(void *args){
    int socket_fd = ((struct thread_info*)args)->socket_fd;
    unsigned int addrlen = sizeof(struct sockaddr_in);

    int len; // variable used to monitor the number of bytes sent
    unsigned char type_to_send;

    STATE read_cur_state;
    unsigned int read_cur_ack_num;

    // store ack to be sent to client side
    struct mtcp_packet send_packet;

    while(1) {
        // waiting for receiving thread to wake sending thread up
        pthread_mutex_lock(&send_thread_sig_mutex);
        pthread_cond_wait(&send_thread_sig, &send_thread_sig_mutex);
        pthread_mutex_unlock(&send_thread_sig_mutex);

        pthread_mutex_lock(&info_mutex);
        read_cur_state = cur_state;
        read_cur_ack_num = cur_ack_num;
        pthread_mutex_unlock(&info_mutex);

        switch (read_cur_state) {
            case THREE_WAY_HANDSHAKE_STATE:
                type_to_send = SYN_ACK;
                encode_mtcp_header(&send_packet, type_to_send, read_cur_ack_num);
                if ((len = sendto(socket_fd, &send_packet, 4, 0, (struct sockaddr*)&client_addr, addrlen)) != 4) {
                    perror("Sending Thread: Sending Thread fails to send SYN_ACK to client");
                }
                printf_helper_send_with_seq(read_cur_state, "Sent packet to client", read_cur_ack_num, "SYN_ACK");
            break;

            case DATA_TRANSMISSION_STATE:
                type_to_send = ACK;
                encode_mtcp_header(&send_packet, type_to_send, read_cur_ack_num);
                if ((len = sendto(socket_fd, &send_packet, 4, 0, (struct sockaddr*)&client_addr, addrlen)) != 4) {
                    perror("Sending Thread: Sending Thread fails to send ACK to client");
                }
                printf_helper_send_with_seq(read_cur_state, "Sent packet to client", read_cur_ack_num, "ACK");
            break;

            case FOUR_WAY_HANDSHAKE_STATE:
                type_to_send = FIN_ACK;
                encode_mtcp_header(&send_packet, type_to_send, read_cur_ack_num);
                if ((len = sendto(socket_fd, &send_packet, 4, 0, (struct sockaddr*)&client_addr, addrlen)) != 4) {
                    perror("Sending Thread: Sending Thread fails to send FIN_ACK to client");
                }
                printf_helper_send_with_seq(read_cur_state, "Sent packet to client", read_cur_ack_num, "FIN_ACK");
                exit(0);
            break;
            default:
            break;
        }
    }
}

static void *receive_thread(void *args){
    int len; // variable used to monitor the number of bytes recv
    unsigned int seq_recv;
    unsigned char type_recv;
    int socket_fd = ((struct thread_info*)args)->socket_fd;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    
    // store recv data from client side, write by recvfrom() in recv thread
    struct mtcp_packet recv_packet;

    STATE read_cur_state;
    unsigned int read_cur_ack_num;
    unsigned int read_local_buf_len;

    while (1) {
        printf_helper_recv_no_seq(cur_state, "listening to message from client");
        if ((len = recvfrom(socket_fd, (char*)&recv_packet, MAX_BUF_SIZE, 0, (struct sockaddr*)&client_addr, &addrlen)) < 4) {
            perror("Server receives incorrect data from server\n");
            pthread_mutex_lock(&info_mutex);
            recv_from_len = len;
            pthread_mutex_unlock(&info_mutex);
            exit(-1);
        }
        len = len - 4;
        pthread_mutex_lock(&info_mutex);
        read_cur_state = cur_state;
        read_cur_ack_num = cur_ack_num;
        read_local_buf_len = local_recv_buf_len;
        pthread_mutex_unlock(&info_mutex);

        decode_mtcp_header(&recv_packet, &type_recv, &seq_recv);
        // printf("%d\n", int(type_recv));
        switch(type_recv) {
            case SYN:
                // SYN, Go to THREE WAY HANDSHAKE
                pthread_mutex_lock(&info_mutex);
                cur_state = THREE_WAY_HANDSHAKE_STATE;
                cur_ack_num = seq_recv + 1;
                printf_helper_recv_with_seq(cur_state, "received packet", seq_recv, "SYN");
                pthread_mutex_unlock(&info_mutex);

                pthread_mutex_lock(&send_thread_sig_mutex);
                pthread_cond_signal(&send_thread_sig);
                pthread_mutex_unlock(&send_thread_sig_mutex);
            break;
            case ACK:
                switch(read_cur_state) {
                    case THREE_WAY_HANDSHAKE_STATE:
                        pthread_mutex_lock(&info_mutex);
                        cur_state = DATA_TRANSMISSION_STATE;
                        printf_helper_recv_with_seq(read_cur_state, "received packet", seq_recv, "ACK");
                        pthread_mutex_unlock(&info_mutex);
                    break;

                    case FOUR_WAY_HANDSHAKE_STATE:
                        // ACK IN FOUR WAY HANDSHAKE
                        printf_helper_recv_with_seq(FOUR_WAY_HANDSHAKE_STATE, "wake up application thread if sleeping", seq_recv, "ACK");
                        // client closed, server to be closed
                        pthread_mutex_lock(&info_mutex);
                        printf_helper_recv_with_seq(cur_state, "received packet", seq_recv, "ACK(FOUR_WAY_HANDSHAKE_STATE)");
                        cur_state = INITIAL_STATE;
                        pthread_mutex_unlock(&info_mutex);
                        // wake up application thread
                        pthread_mutex_lock(&app_thread_sig_mutex);
                        pthread_cond_signal(&app_thread_sig);
                        pthread_mutex_unlock(&app_thread_sig_mutex);
                        exit(0);
                    break;
                    default:
                    break;
                }
            break;

            case DATA:
                if(seq_recv == read_cur_ack_num){
                    // save this packet and wake up sending thread to send ack
                    pthread_mutex_lock(&local_recv_buf_mutex);
                    memcpy(&(local_recv_buf[read_local_buf_len]),get_data(&recv_packet),len);
                    pthread_mutex_unlock(&local_recv_buf_mutex);

                    pthread_mutex_lock(&info_mutex);
                    cur_ack_num += len;
                    local_recv_buf_len += len;
                    pthread_mutex_unlock(&info_mutex);

                    pthread_mutex_lock(&send_thread_sig_mutex);
                    pthread_cond_signal(&send_thread_sig);
                    pthread_mutex_unlock(&send_thread_sig_mutex);
                }
                else if (seq_recv < read_cur_ack_num) {
                    // don't save the packet but wake up sending thread and send ack again
                    pthread_mutex_lock(&send_thread_sig_mutex);
                    pthread_cond_signal(&send_thread_sig);
                    pthread_mutex_unlock(&send_thread_sig_mutex); 
                }
                // else discard the out-of-order packet
          
            break;

            case FIN:
                // FIN, Go to FOUR WAY HANDSHAKE
                pthread_mutex_lock(&info_mutex);
                cur_ack_num += (len + 1);
                cur_state = FOUR_WAY_HANDSHAKE_STATE;
                pthread_mutex_unlock(&info_mutex);
                printf_helper_recv_with_seq(cur_state, "recv packet", seq_recv, "FIN");
                if( len > 0) {
                    pthread_mutex_lock(&local_recv_buf_mutex);
                    memcpy(&(local_recv_buf[read_local_buf_len]),get_data(&recv_packet),len);
                    pthread_mutex_unlock(&local_recv_buf_mutex);
                    pthread_mutex_lock(&info_mutex);
                    local_recv_buf_len += len;
                    pthread_mutex_unlock(&info_mutex);
                }
                pthread_mutex_lock(&send_thread_sig_mutex);
                pthread_cond_signal(&send_thread_sig);
                pthread_mutex_unlock(&send_thread_sig_mutex);
            break;

            default:
            break;
        }
    }
}

void mtcp_accept(int socket_fd, struct sockaddr_in *server_addr){
    struct thread_info args;
    args.socket_fd = socket_fd;
    args.server_addr = server_addr;
    pthread_mutex_lock(&info_mutex);
    STATE read_cur_state = cur_state;
    pthread_mutex_unlock(&info_mutex);
    if (read_cur_state != INITIAL_STATE) {
        perror("mtcp_accept: cur_state is not INITIAL_STATE!\n");
        return;
    } 
    if (pthread_create(&send_thread_pid, NULL, &send_thread, &args)) {
        perror("Fail to create sending thread");
    }
    if (pthread_create(&recv_thread_pid, NULL, &receive_thread, &args)) {
        perror("Fail to create receving thread");
    }

    struct timespec timeToWait;
    struct timeval now;
    while(1) {
        gettimeofday(&now, NULL);
        timeToWait.tv_sec = now.tv_sec + 1;

        pthread_mutex_lock(&app_thread_sig_mutex);
        pthread_cond_timedwait(&app_thread_sig, &app_thread_sig_mutex, &timeToWait);
        pthread_mutex_unlock(&app_thread_sig_mutex);

        pthread_mutex_lock(&info_mutex);
        read_cur_state = cur_state;
        pthread_mutex_unlock(&info_mutex);
        if (read_cur_state == DATA_TRANSMISSION_STATE) {
            break;
        }
    }
    printf_helper_app("DATA_TRANSMISSION_STATE", "mtcp_accept return");
    return;
}

int mtcp_read(int socket_fd, unsigned char *buf, int buf_len){
    STATE read_cur_state;
    unsigned int read_local_buf_len;
    int read_recv_from_len; // used to detech wheter recvfrom has return -1
    struct timespec timeToWait;
    struct timeval now;
    while(1){
        gettimeofday(&now, NULL);
        timeToWait.tv_sec = now.tv_sec + 1;

        pthread_mutex_lock(&app_thread_sig_mutex);
        pthread_cond_timedwait(&app_thread_sig, &app_thread_sig_mutex, &timeToWait);
        pthread_mutex_unlock(&app_thread_sig_mutex);

        pthread_mutex_lock(&info_mutex);
        read_cur_state = cur_state;
        read_recv_from_len = recv_from_len;
        read_local_buf_len = local_recv_buf_len;
        pthread_mutex_unlock(&info_mutex);
        if (read_recv_from_len == -1) {
            return -1;
        } else if ((read_cur_state == FOUR_WAY_HANDSHAKE_STATE) && (read_local_buf_len == app_thread_read_len)) {
            return 0;
        } else if (read_cur_state != DATA_TRANSMISSION_STATE) {
            return -1;
        } else {
            unsigned int size_to_recv = min(buf_len, read_local_buf_len - app_thread_read_len);
            if ( size_to_recv > 0) {
                pthread_mutex_lock(&local_recv_buf_mutex);
                memcpy(buf, &(local_recv_buf[local_recv_buf_len]), size_to_recv);
                pthread_mutex_unlock(&local_recv_buf_mutex);
                app_thread_read_len += size_to_recv;
                return size_to_recv;
            }
        }
    }
}

void mtcp_close(int socket_fd){
    STATE read_cur_state;

    pthread_mutex_lock(&info_mutex);
    read_cur_state = cur_state;
    pthread_mutex_unlock(&info_mutex);
    if (read_cur_state == INITIAL_STATE) {
        return;
    }
    struct timespec timeToWait;
    struct timeval now;
    while(1){
        gettimeofday(&now, NULL);
        timeToWait.tv_sec = now.tv_sec + 1;
        pthread_mutex_lock(&app_thread_sig_mutex);
        pthread_cond_timedwait(&app_thread_sig, &app_thread_sig_mutex, &timeToWait);
        pthread_mutex_unlock(&app_thread_sig_mutex);

        pthread_mutex_lock(&info_mutex);
        read_cur_state = cur_state;
        pthread_mutex_unlock(&info_mutex);
        if (read_cur_state == INITIAL_STATE) {
            break;
        }
    }
    close(socket_fd);

    return;
}
