#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
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

#define SYN     0x00
#define SYN_ACK 0x01
#define FIN     0x02
#define FIN_ACK 0x03
#define ACK     0x04
#define DATA    0x05

enum STATE {
    THREE_WAY_HANDSHAKE,
    DATA_TRANSMISSION,
    FOUR_WAY_HANDSHAKE
};

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

void printf_helper(const char* thread_name, const char* message) {
    printf("[%s]: %s\n" ANSI_COLOR_RESET, thread_name, message);
}

void printf_helper_color(const char* color, const char* thread_name, const char* message) {
    printf("%s[%s]: %s\n", color, thread_name, message);
}

/* -------------------- Global Variables -------------------- */

/* ThreadID for Sending Thread and Receiving Thread */
static pthread_t send_thread_pid;
static pthread_t recv_thread_pid;

static pthread_cond_t app_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t app_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t send_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;
static enum STATE state = THREE_WAY_HANDSHAKE;
static unsigned char last_packet_sent = 0x06;
static unsigned char last_packet_recv = 0x06;
static unsigned int seq = 0;

struct thread_info {
    int socket_fd;
    struct sockaddr_in* server_addr;
};

static void *send_thread(void *args){
    int socket_fd = ((struct thread_info*)args)->socket_fd;
    struct sockaddr_in *server_addr = ((struct thread_info*)args)->server_addr;
    unsigned int addrlen = sizeof(struct sockaddr_in);

    struct timespec timeToWait;
    struct timeval now;
    timeToWait.tv_nsec = 0;
    //timeToWait.tv_nsec = now.tv_usec * 1000UL;

    struct mtcp_header header;
    unsigned char mode_to_send;
    unsigned int seq_to_send;
    int len;

    // AT START UP, WAIT FOR APPLICATION THREAD TO WAKE ME UP
    pthread_mutex_lock(&send_thread_sig_mutex);
    pthread_cond_wait(&send_thread_sig, &send_thread_sig_mutex);
    pthread_mutex_unlock(&send_thread_sig_mutex);

    // GET GLOBAL INFO
    pthread_mutex_lock(&info_mutex);
    pthread_mutex_unlock(&info_mutex);

    // SEND SYN PACKAET
    mode_to_send = SYN;
    seq_to_send = 0;
    encode_mtcp_header(&header, mode_to_send, seq_to_send);
    if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr *)server_addr, addrlen)) != 4) {
        perror("Sending thread: Client fails to send SYN to server");
    }
    printf_helper_color(SEND_THREAD_COLOR, "Sending thread", "sleep after sending SYN"); 
    pthread_mutex_lock(&send_thread_sig_mutex);
    pthread_cond_wait(&send_thread_sig, &send_thread_sig_mutex);
    pthread_mutex_unlock(&send_thread_sig_mutex);
    printf_helper_color(SEND_THREAD_COLOR, "Sending thread","wake from sending SYN");


    mode_to_send = ACK;
    seq_to_send = 1;
    encode_mtcp_header(&header, mode_to_send, seq_to_send);
    if ((len = sendto(socket_fd, &header, 4, 0, (struct sockaddr *)server_addr, addrlen)) != 4) {
        perror("Client fails to send ACK to server\n");
    }
    printf_helper_color(SEND_THREAD_COLOR, "Sending thread", "terminate after sending ACK to server");

    pthread_mutex_lock(&app_thread_sig_mutex);
    pthread_cond_signal(&app_thread_sig);
    pthread_mutex_unlock(&app_thread_sig_mutex);

    return;
}

static void *receive_thread(void *args){
    int len;
    unsigned int ack_recv;
    unsigned char mode_recv;

    int socket_fd = ((struct thread_info*)args)->socket_fd;
    struct sockaddr_in *server_addr = ((struct thread_info*)args)->server_addr;

    struct mtcp_header header;
    if ((len = recvfrom(socket_fd, (char*)&header, 4, 0, NULL, NULL)) != 4) {
        perror("Client receives incorrect data from server\n");
    }
    decode_mtcp_header(&header, &mode_recv, &ack_recv);

    // critical section
    pthread_mutex_lock(&info_mutex);
    pthread_mutex_unlock(&info_mutex);

    if (mode_recv != SYN_ACK) {
        perror("Client expects SYN_ACK\n");
    }
    printf_helper_color(RECV_THREAD_COLOR, "Receiving thread", "wake up sending thread after receiving SYN_ACK");
    pthread_mutex_lock(&send_thread_sig_mutex);
    pthread_cond_signal(&send_thread_sig);
    pthread_mutex_unlock(&send_thread_sig_mutex);
    printf_helper_color(RECV_THREAD_COLOR, "Receiving thread", "terminate aftering waking up sending thread");

    return;
}

/* Connect Function Call (mtcp Version) */
void mtcp_connect(int socket_fd, struct sockaddr_in *server_addr){
    struct thread_info args;
    args.socket_fd = socket_fd;
    args.server_addr = server_addr;
    if (pthread_create(&send_thread_pid, NULL, &send_thread, &args)) {
        perror("Fail to create sending thread");
    }
    if (pthread_create(&recv_thread_pid, NULL, &receive_thread, &args)) {
        perror("Fail to create receving thread");
    }
    sleep(1);

    printf_helper_color(APP_THREAD_COLOR, "Application thread", "wake sending thread after creating sending thread and receiving thread");
    pthread_mutex_lock(&send_thread_sig_mutex);
    pthread_cond_signal(&send_thread_sig);
    pthread_mutex_unlock(&send_thread_sig_mutex);

    printf_helper_color(APP_THREAD_COLOR, "Application thread", "sleep after waking sending thread up");
    pthread_mutex_lock(&app_thread_sig_mutex);
    pthread_cond_wait(&app_thread_sig, &app_thread_sig_mutex);
    pthread_mutex_unlock(&app_thread_sig_mutex);
    printf_helper_color(APP_THREAD_COLOR, "Application thread", "THREE WAY HANDSHAKE complete!");
    printf_helper_color(APP_THREAD_COLOR, "Application thread", "mtcp connect return");
}

/* Write Function Call (mtcp Version) */
int mtcp_write(int socket_fd, unsigned char *buf, int buf_len){
    ;
}

/* Close Function Call (mtcp Version) */
void mtcp_close(int socket_fd){
    ;
}
