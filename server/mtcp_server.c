#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
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

typedef enum{
    INITIAL_STATE,
    THREE_WAY_HANDSHAKE_STATE,
    DATA_TRANSMISSION_STATE,
    FOUR_WAY_HANDSHAKE_STATE
} STATE;

struct mtcp_header {
    unsigned char header_[4];
    unsigned char data[1000];
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
    memset(header->data, 0, 1000);
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

void printf_helper_send_with_seq(const STATE state, const char* message, int seq, const char* mode) {
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
    printf("%s[%s][%s]: %s [seq:%d][mode:%s]\n%s", SEND_THREAD_COLOR, "Sending Thread", state_msg, message, seq, mode, ANSI_COLOR_RESET);
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

void printf_helper_recv_with_seq(const STATE state, const char* message, int seq, const char* mode) {
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
    printf("%s[%s][%s]: %s [seq:%d][mode:%s]\n%s", RECV_THREAD_COLOR, "Receiving Thread", state_msg, message, seq, mode, ANSI_COLOR_RESET);
}

/* ThreadID for Sending Thread and Receiving Thread */
static pthread_t send_thread_pid;
static pthread_t recv_thread_pid;

static pthread_cond_t app_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t app_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t send_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool closed = false;
static int cur_seq = -1;
// static char last_packet_sent = INITIAL_MODE;
static char last_packet_recv = INITIAL_MODE;
static STATE cur_state = INITIAL_STATE;

struct thread_info {
    int socket_fd;
    struct sockaddr_in* server_addr;
};

struct sockaddr_in client_addr;

static void *send_thread(void *args){
    int socket_fd = ((struct thread_info*)args)->socket_fd;
    unsigned int addrlen = sizeof(struct sockaddr_in);
    struct mtcp_header header;

    int len; // variable used to monitor the number of bytes sent
    unsigned char mode_to_send;
    unsigned int seq_to_send;
    STATE read_state;
    char read_last_packet_recv;

    bool connection_in_use = true;
    while(connection_in_use) {
        pthread_mutex_lock(&send_thread_sig_mutex);
        pthread_cond_wait(&send_thread_sig, &send_thread_sig_mutex);
        pthread_mutex_unlock(&send_thread_sig_mutex);

        pthread_mutex_lock(&info_mutex);
        read_state = cur_state;
        read_last_packet_recv = last_packet_recv;
        pthread_mutex_unlock(&info_mutex);

        switch (read_state) {
            case THREE_WAY_HANDSHAKE_STATE:
                if (read_last_packet_recv != SYN) {
                    perror("Sending Thread: expects last packet received in THREE_WAY_HANDSHAKE_STATE to be SYN\n");
                } else {
                    mode_to_send = SYN_ACK;
                    seq_to_send = 0;
                    encode_mtcp_header(&header, mode_to_send, seq_to_send);
                    if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr*)&client_addr, addrlen)) != 4) {
                        perror("Sending Thread: Sending Thread fails to send SYN_ACK to client");
                    }
                    seq_to_send += 1;
                    printf_helper_send_with_seq(read_state, "Sent packet to client", seq_to_send, "SYN_ACK");
                }
            break;

            case DATA_TRANSMISSION_STATE:
            break;

            case FOUR_WAY_HANDSHAKE_STATE:
                if (read_last_packet_recv != FIN) {
                    perror("Sending Thread: expects last packet received in FOUR_WAY_HANDSHAKE_STATE to be FIN\n");
                } else {
                    mode_to_send = FIN_ACK;
                    encode_mtcp_header(&header, mode_to_send, seq_to_send);
                    if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr*)&client_addr, addrlen)) != 4) {
                        perror("Sending Thread: Sending Thread fails to send FIN_ACK to client");
                    }
                    seq_to_send += 1;
                    connection_in_use = false;
                    printf_helper_send_with_seq(read_state, "Sent packet to client", seq_to_send, "FIN_ACK");
                }
            break;
        }
    }
}

static void *receive_thread(void *args){
    int len; // variable used to monitor the number of bytes sent
    unsigned int seq_recv;
    unsigned char mode_recv;
    int socket_fd = ((struct thread_info*)args)->socket_fd;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    struct mtcp_header header;

    bool monitoring = true;
    while (monitoring) {
        printf_helper_recv_no_seq(cur_state, "listening to message from client");
        if ((len = recvfrom(socket_fd, (char*)&header, 4, 0, (struct sockaddr*)&client_addr, &addrlen)) < 4) {
            perror("Server receives incorrect data from server\n");
            exit(-1);
        }
        decode_mtcp_header(&header, &mode_recv, &seq_recv);
        switch(mode_recv) {
            case SYN:
                // SYN, Go to THREE WAY HANDSHAKE
                printf_helper_recv_with_seq(cur_state, "wake up sending thread", seq_recv, "SYN");
                pthread_mutex_lock(&info_mutex);
                last_packet_recv = mode_recv;
                cur_state = THREE_WAY_HANDSHAKE_STATE;
                cur_seq = seq_recv;
                pthread_mutex_unlock(&info_mutex);
                pthread_mutex_lock(&send_thread_sig_mutex);
                pthread_cond_signal(&send_thread_sig);
                pthread_mutex_unlock(&send_thread_sig_mutex);
            break;
            case ACK:
                switch(cur_state) {
                    case THREE_WAY_HANDSHAKE_STATE:
                        // ACK IN THREE WAY HANDSHAKE
                        printf_helper_recv_with_seq(cur_state, "wake up application thread", seq_recv, "ACK");
                        pthread_mutex_lock(&info_mutex);
                        last_packet_recv = mode_recv;
                        cur_seq = seq_recv;
                        cur_state = DATA_TRANSMISSION_STATE;
                        pthread_mutex_unlock(&info_mutex);
                        pthread_mutex_lock(&app_thread_sig_mutex);
                        pthread_cond_signal(&app_thread_sig);
                        pthread_mutex_unlock(&app_thread_sig_mutex);
                    break;

                    case DATA_TRANSMISSION_STATE:
                    break;

                    case FOUR_WAY_HANDSHAKE_STATE:
                        // ACK IN FOUR WAY HANDSHAKE
                        printf_helper_recv_with_seq(THREE_WAY_HANDSHAKE_STATE, "wake up application thread if sleeping", seq_recv, "ACK");
                        pthread_mutex_lock(&info_mutex);
                        bool wake_app_thread = false;
                        if (closed == false)
                            wake_app_thread = true;
                        last_packet_recv = mode_recv;
                        cur_seq = seq_recv;
                        pthread_mutex_unlock(&info_mutex);
                        if (wake_app_thread) {
                            pthread_mutex_lock(&app_thread_sig_mutex);
                            pthread_cond_signal(&app_thread_sig);
                            pthread_mutex_unlock(&app_thread_sig_mutex);
                        }
                        monitoring = false;
                    break;
                }
            break;

            case DATA:
            break;

            case FIN:
                // FIN, Go to FOUR WAY HANDSHAKE
                printf_helper_recv_with_seq(cur_state, "wake up sending thread", seq_recv, "FIN");
                pthread_mutex_lock(&info_mutex);
                last_packet_recv = mode_recv;
                cur_state = FOUR_WAY_HANDSHAKE_STATE;
                cur_seq = seq_recv;
                pthread_mutex_unlock(&info_mutex);
                pthread_mutex_lock(&send_thread_sig_mutex);
                pthread_cond_signal(&send_thread_sig);
                pthread_mutex_unlock(&send_thread_sig_mutex);
            break;
        }
    }
}

void mtcp_accept(int socket_fd, struct sockaddr_in *server_addr){
    struct thread_info args;
    args.socket_fd = socket_fd;
    args.server_addr = server_addr;
    if (pthread_create(&send_thread_pid, NULL, &send_thread, &args)) {
        perror("Fail to create sending thread");
    }
    if (pthread_create(&recv_thread_pid, NULL, &receive_thread, &args)) {
        perror("Fail to create receving thread");
    }
    printf_helper_app("INITIAL_STATE", "sleep after creating sending thread and receiving thread");
    pthread_mutex_lock(&app_thread_sig_mutex);
    pthread_cond_wait(&app_thread_sig, &app_thread_sig_mutex);
    pthread_mutex_unlock(&app_thread_sig_mutex);
    printf_helper_app("THREE_WAY_HANDSHAKE_STATE", "wake up and mtcp_accept return");

    return;
}

int mtcp_read(int socket_fd, unsigned char *buf, int buf_len){
    ;
}

void mtcp_close(int socket_fd){
    ;
}
