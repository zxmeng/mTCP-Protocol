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

#define FILE_MAX_SIZE ((1 << 28) - 3)

unsigned int min(unsigned int a, unsigned int b) {
    return a > b ? b : a;
}

// unsigned char global_send_buf[MAX_BUF_SIZE];
// unsigned char global_recv_buf[MAX_BUF_SIZE];

/* -------------------- Global Variables -------------------- */

/* ThreadID for Sending Thread and Receiving Thread */
static pthread_t send_thread_pid;
static pthread_t recv_thread_pid;

static pthread_cond_t app_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t app_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t send_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t local_send_buf_mutex = PTHREAD_MUTEX_INITIALIZER;

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

/* three indicators to determine whether sending thread timeout*/
static unsigned int last_seq_num = -1;    // modified by SENDING thread
static unsigned int cur_seq_num = 0;     // modified by RECVING thread
static unsigned int next_seq_num = 0;    // modified by SENDING thread

static unsigned char last_type_sent = INITIAL_MODE; // modified by SENDING thread
static unsigned char last_type_recv = INITIAL_MODE; // modified by RECVING thread

static STATE cur_state = INITIAL_STATE;    // modified by APP thread connect() and close()

static unsigned char local_send_buf[FILE_MAX_SIZE]; // modified by APP thread write()
static unsigned int local_send_buf_len = 0;    // modified by APP thread wrtie(), watched by SENDING thread

static int send_to_len = 0; // return value of sendTo system call

struct mtcp_packet {
    unsigned char header_[4];
    unsigned char data_[SEGMENT_SIZE];
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

void empty_mtcp_header(struct mtcp_packet* packet) {
    memset(packet, 0, sizeof(struct mtcp_packet));
}

void put_data(struct mtcp_packet* packet, unsigned char* data, unsigned int size) {
    memset(packet->data_, 0, SEGMENT_SIZE);
    memcpy(packet->data_, data, size);
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
        default:
        break;
    }
    printf("%s[%s][%s]: %s\n%s", SEND_THREAD_COLOR, "Sending Thread", state_msg, message, ANSI_COLOR_RESET);
}

void printf_helper_send_with_seq(const STATE state, const char* message, int seq, char* type) {
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
        default:
        break;
    }
    printf("%s[%s][%s]: %s [sent seq:%d][sent type:%s]\n%s", SEND_THREAD_COLOR, "Sending Thread", state_msg, message, seq, type, ANSI_COLOR_RESET);
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
        default:
        break;
    }
    printf("%s[%s][%s]: %s\n%s", RECV_THREAD_COLOR, "Receving Thread", state_msg, message, ANSI_COLOR_RESET);
}

void printf_helper_recv_with_seq(const STATE state, const char* message, int seq, char* type) {
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
        default:
        break;
    }
    printf("%s[%s][%s]: %s [recv ack:%d][recv type:%s]\n%s", RECV_THREAD_COLOR, "Receiving Thread", state_msg, message, seq, type, ANSI_COLOR_RESET);
}

static void *send_thread(void *args){
    // socket information
    int socket_fd = ((struct thread_info*)args)->socket_fd;
    struct sockaddr_in *server_addr = ((struct thread_info*)args)->server_addr;
    unsigned int addrlen = sizeof(struct sockaddr_in);

    int len;                                 // variable used to monitor the number of bytes sent
    struct mtcp_packet packet;                  
    unsigned char type_to_send;              // option sent to server side
        
        
    // check the status     
    STATE read_cur_state;                    // local version of global cur_state
        
    unsigned int read_last_seq_num;          // local version of global last_seq_num
    unsigned int read_cur_seq_num;           // local version of global cur_seq_num
    unsigned int read_next_seq_num;          // local version of global next_seq_num
        
    unsigned char read_last_type_recv;       // local version of last_type_recv
    unsigned char read_last_type_sent;       // local version of last_type_sent

    unsigned int read_local_send_buf_len;    // local version of local_send_buf_len

    struct timespec timeToWait;
    struct timeval now;

    int size_to_send;

    while(1) {
        // get current time and time to wait
        gettimeofday(&now, NULL);
        timeToWait.tv_sec = now.tv_sec + 1;

        pthread_mutex_lock(&send_thread_sig_mutex);
        pthread_cond_timedwait(&send_thread_sig, &send_thread_sig_mutex, &timeToWait);
        pthread_mutex_unlock(&send_thread_sig_mutex);

        pthread_mutex_lock(&info_mutex);

        read_cur_state = cur_state;

        read_last_seq_num = last_seq_num;
        read_cur_seq_num = cur_seq_num; 
        read_next_seq_num = next_seq_num;

        read_last_type_recv = last_type_recv;
        read_last_type_sent = last_type_sent;

        read_local_send_buf_len = local_send_buf_len;
        
        pthread_mutex_unlock(&info_mutex);

        switch(read_cur_state) {
            case INITIAL_STATE:
            break;

            case THREE_WAY_HANDSHAKE_STATE:
                // read_last_type_recv = INITIAL_MODE, SYN_ACK
                switch (read_last_type_recv) {
                    case INITIAL_MODE:
                        type_to_send = SYN;
                        empty_mtcp_header(&packet);
                        encode_mtcp_header(&packet, type_to_send, read_cur_seq_num);
                        if ((len = sendto(socket_fd, &packet, 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                            printf("Sending thread: Client fails to send SYN to server\n");
                            pthread_mutex_lock(&info_mutex);
                            send_to_len = -1;
                            pthread_mutex_unlock(&info_mutex);
                            pthread_exit(NULL);
                        }
                        pthread_mutex_lock(&info_mutex);
                        last_type_sent = SYN;
                        last_seq_num = cur_seq_num; // last_seq_num = cur_seq_num = 0;
                        next_seq_num += 1; // next_seq_num = 1;
                        pthread_mutex_unlock(&info_mutex);
                        printf_helper_send_with_seq(read_cur_state, "sent packet", read_cur_seq_num, "SYN");
                    break;
                    case SYN_ACK:
                        type_to_send = ACK;
                        empty_mtcp_header(&packet);
                        encode_mtcp_header(&packet, type_to_send, read_cur_seq_num);
                        if ((len = sendto(socket_fd, &packet, 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                            printf("Sending thread: Client fails to send ACK to server\n");
                            pthread_mutex_lock(&info_mutex);
                            send_to_len = -1;
                            pthread_mutex_unlock(&info_mutex);
                            pthread_exit(NULL);
                        }
                        pthread_mutex_lock(&info_mutex);
                        last_type_sent = ACK;
                        // last_seq_num = cur_seq_num; // last_seq_num = cur_seq_num = 1;
                        // next_seq_num += 1; // next_seq_num = 1;
                        pthread_mutex_unlock(&info_mutex);

                        printf_helper_send_with_seq(read_cur_state, "sent packet", read_cur_seq_num, "ACK");

                        pthread_mutex_lock(&app_thread_sig_mutex);
                        pthread_cond_signal(&app_thread_sig);
                        pthread_mutex_unlock(&app_thread_sig_mutex);
                    break;
                    default:
                        printf("Sending Thread: expects last packet received in THREE_WAY_HANDSHAKE_STATE to be INITIAL or SYN_ACK\n");
                        exit(-1);
                    break;
                }
            break;

            case DATA_TRANSMISSION_STATE:
                switch(read_last_type_recv) {
                    case SYN_ACK:
                        switch(read_last_type_sent) {
                            case ACK:
                                // in between SYN_ACK (received) and ACK(send) and has data to send
                                if (read_local_send_buf_len > 0) {
                                    // read_next_seq_num = 1;
                                    type_to_send = DATA;
                                    size_to_send = min(SEGMENT_SIZE, read_local_send_buf_len - (read_next_seq_num - 1));
                                    empty_mtcp_header(&packet);
                                    encode_mtcp_header(&packet, type_to_send, read_cur_seq_num);
                                    pthread_mutex_lock(&local_send_buf_mutex);
                                    put_data(&packet, &local_send_buf[read_next_seq_num - 1], size_to_send);
                                    pthread_mutex_unlock(&local_send_buf_mutex);
                                    if ((len = sendto(socket_fd, &packet, size_to_send + 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                                        printf("Sending thread: Client fails to send DATA to server\n");
                                        pthread_mutex_lock(&info_mutex);
                                        send_to_len = -1;
                                        pthread_mutex_unlock(&info_mutex);
                                        pthread_exit(NULL);
                                    }
                                    pthread_mutex_lock(&info_mutex);
                                    last_type_sent = type_to_send;
                                    last_seq_num = next_seq_num; // last_seq_num = 1;
                                    next_seq_num += size_to_send;
                                    pthread_mutex_unlock(&info_mutex);
                                    printf_helper_send_with_seq(read_cur_state, "sent packet", read_cur_seq_num, "DATA");
                                }
                                // in between SYN_ACK (received) and ACK(send) and has no data to send => sleep
                            break;
                            case DATA:
                                // in between SYN_ACK (received) and DATA(send) and time out => retransmit
                                if ((len = sendeto(sockt_fd, &packet, size_to_send + 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                                    printf("Sending thread: Client fails to send DATA to server\n");
                                    pthread_mutex_lock(&info_mutex);
                                    send_to_len = -1;
                                    pthread_mutex_unlock(&info_mutex);
                                    pthread_exit(NULL);
                                }
                                printf_helper_send_with_seq(read_cur_state, "retransmit packet", read_cur_seq_num, "DATA");
                            break;
                            default:
                                printf("Sending Thread: incorrect last packet sent in DATA_TRANSMISSION_STATE, last_type_recv: %d\n", (int)read_last_type_sent);
                                exit(-1);
                            break;
                        }
                    break;
                    case ACK:
                        // read_last_type_sent mut be DATA
                        if (read_last_type_sent != DATA) {
                            printf("Sending Thread: incorrect last packet sent in DATA_TRANSMISSION_STATE, last_type_sent: %d\n", (int)read_last_type_sent);
                            exit(-1);
                        }
                        // retransmit
                        if (read_last_seq_num == read_cur_seq_num) {
                            if ((len = sendto(socket_fd, &packet, size_to_send + 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                                printf("Sending thread: Client fails to send DATA to server\n");
                                pthread_mutex_lock(&info_mutex);
                                send_to_len = -1;
                                pthread_mutex_unlock(&info_mutex);
                                pthread_exit(NULL);
                            }
                        // check if there is new data to send, if yes => send new data.  if not => sleep
                        } else if (read_local_send_buf_len > (read_next_seq_num - 1)) {
                            type_to_send = DATA;
                            size_to_send = min(SEGMENT_SIZE, read_local_send_buf_len - (read_next_seq_num - 1));
                            empty_mtcp_header(&packet);
                            encode_mtcp_header(&packet, type_to_send, read_cur_seq_num);
                            pthread_mutex_lock(&local_send_buf_mutex);
                            put_data(&packet, &local_send_buf[read_next_seq_num - 1], size_to_send);
                            pthread_mutex_unlock(&local_send_buf_mutex);
                            if ((len = sendto(socket_fd, &packet, size_to_send + 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                                printf("Sending thread: Client fails to send DATA to server\n");
                                pthread_mutex_lock(&info_mutex);
                                send_to_len = -1;
                                pthread_mutex_unlock(&info_mutex);
                                pthread_exit(NULL);
                            }
                            pthread_mutex_lock(&info_mutex);
                            last_type_sent = type_to_send;
                            last_seq_num = cur_seq_num;
                            next_seq_num += size_to_send;
                            pthread_mutex_unlock(&info_mutex);
                            printf_helper_send_with_seq(read_cur_state, "sent packet", read_cur_seq_num, "DATA");
                        }
                    break;
                    default:
                        printf("Sending Thread: incorrect last packet received in DATA_TRANSMISSION_STATE, last_type_recv: %d\n", (int)read_last_type_recv);
                        exit(-1);
                    break;
                }
            break;

            case FOUR_WAY_HANDSHAKE_STATE:
                switch(read_last_type_recv) {
                    // This ACK is from the last state (Data transmission state), hence retransmit or send FIN or FIN_ACK
                    case SYN_ACK:
                    case ACK:
                        // retransmit
                        if (read_last_seq_num == read_cur_seq_num) {
                            if ((len = sendto(socket_fd, &packet, 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                                printf("Sending thread: Client fails to send DATA to server\n");
                                pthread_mutex_lock(&info_mutex);
                                send_to_len = -1;
                                pthread_mutex_unlock(&info_mutex);
                                pthread_exit(NULL);
                            }
                            printf_helper_send_with_seq(read_cur_state, "retransmit packet", read_cur_seq_num, "DATA");
                        // still has data to send, check if size of remaining data > 1000, if yes => send DATA if no => FIN_DATA , if size == 0 => FIN
                        } else {
                            size_to_send = read_local_send_buf_len - (read_next_seq_num - 1);
                            // size_to_send < 0 => sleep
                            if (size_to_send < 0) {
                                break;
                            // size_to_send == 0 => send FIN
                            } else if (size_to_send == 0) {
                                type_to_send = FIN;
                                empty_mtcp_header(&packet);
                                encode_mtcp_header(&packet, type_to_send, read_cur_seq_num);
                                if ((len = sendto(socket_fd, &packet, 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                                    printf("Sending thread: Client fails to send FIN to server\n");
                                    pthread_mutex_lock(&info_mutex);
                                    send_to_len = -1;
                                    pthread_mutex_unlock(&info_mutex);
                                    pthread_exit(NULL);
                                }
                                pthread_mutex_lock(&info_mutex);
                                last_type_sent = type_to_send;
                                last_seq_num = cur_seq_num;
                                next_seq_num += 1;
                                pthread_mutex_unlock(&info_mutex);
                                printf_helper_send_with_seq(read_cur_state, "send packet", read_cur_seq_num, "FIN");
                            //  0 < size_to_send <= SEGMENT_SIZE => send FIN_DATA
                            } else if (size_to_send <= SEGMENT_SIZE) {
                                type_to_send = FIN;
                                empty_mtcp_header(&packet);
                                encode_mtcp_header(&packet, type_to_send, read_cur_seq_num);
                                pthread_mutex_lock(&local_send_buf_mutex);
                                put_data(&packet, &local_send_buf[read_next_seq_num - 1], size_to_send);
                                pthread_mutex_unlock(&local_send_buf_mutex);
                                if ((len = sendto(socket_fd, &packet, size_to_send + 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                                    printf("Sending thread: Client fails to send FIN to server\n");
                                    pthread_mutex_lock(&info_mutex);
                                    send_to_len = -1;
                                    pthread_mutex_unlock(&info_mutex);
                                    pthread_exit(NULL);
                                }
                                pthread_mutex_lock(&info_mutex);
                                last_type_sent = type_to_send;
                                last_seq_num = next_seq_num;
                                next_seq_num += (size_to_send + 1);
                                pthread_mutex_unlock(&info_mutex);
                                printf_helper_send_with_seq(read_cur_state, "sent packet", read_cur_seq_num, "FIN_DATA");
                            // size_to_send > SEGMENT_SIZE => send DATA packet
                            } else {
                                type_to_send = DATA;
                                size_to_send = SEGMENT_SIZE;
                                empty_mtcp_header(&packet);
                                encode_mtcp_header(&packet, type_to_send, read_cur_seq_num);
                                pthread_mutex_lock(&local_send_buf_mutex);
                                put_data(&packet, &local_send_buf[read_next_seq_num - 1], size_to_send);
                                pthread_mutex_unlock(&local_send_buf_mutex);
                                if ((len = sendto(socket_fd, &packet, size_to_send + 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                                    printf("Sending thread: Client fails to send FIN to server\n");
                                    pthread_mutex_lock(&info_mutex);
                                    send_to_len = -1;
                                    pthread_mutex_unlock(&info_mutex);
                                    pthread_exit(NULL);
                                }
                                pthread_mutex_lock(&info_mutex);
                                last_type_sent = type_to_send;
                                last_seq_num = next_seq_num;
                                next_seq_num += size_to_send;
                                pthread_mutex_unlock(&info_mutex);
                                printf_helper_send_with_seq(read_cur_state, "sent packet", read_cur_seq_num, "DATA");
                            }
                        }
                    break;

                    case FIN_ACK:
                        type_to_send = ACK;
                        empty_mtcp_header(&packet);
                        encode_mtcp_header(&packet, type_to_send, read_cur_seq_num);
                        if ((len = sendto(socket_fd, &packet, 4, 0, (struct sockaddr *)server_addr, addrlen)) < 0) {
                            printf("Sending thread: Client fails to send ACK to server\n");
                            pthread_mutex_lock(&info_mutex);
                            send_to_len = -1;
                            pthread_mutex_unlock(&info_mutex);
                            pthread_exit(NULL);
                        }
                        pthread_mutex_lock(&info_mutex);
                        last_type_sent = ACK;
                        last_seq_num = cur_seq_num;
                        pthread_mutex_unlock(&info_mutex);
                        printf_helper_send_with_seq(read_cur_state, "sent packet", read_cur_seq_num, "ACK");

                        pthread_mutex_lock(&app_thread_sig_mutex);
                        pthread_cond_signal(&app_thread_sig);
                        pthread_mutex_unlock(&app_thread_sig_mutex);
                        pthread_exit(NULL);
                    break;

                    default:
                        perror("Sending Thread: expects last packet received in FOUR_WAY_HANDSHAKE_STATE to be ACK or FIN_ACK\n");
                        exit(-1);
                    break;
                }
            break;

            default:
                perror("Fatal: Sending thread sees INITIAL_STATE\n");
                exit(-1);
            break;
        }
    }
}

static void *receive_thread(void *args){
    // socket information
    int socket_fd = ((struct thread_info*)args)->socket_fd;

    int len;                                 // variable used to monitor the number of bytes sent
    struct mtcp_packet packet;                  
    unsigned char type_recv;              // option sent to server side
    unsigned int ack_recv;
    
    STATE read_cur_state;
    unsigned int read_next_seq_num;          // local version of global next_seq_num

    while(1) {
        if ((len = recvfrom(socket_fd, (unsigned char*)&packet, 4, 0, NULL, NULL)) < 4) {
            perror("Server receives incorrect data from server\n");
            exit(-1);
        }
        decode_mtcp_header(&packet, &type_recv, &ack_recv);
        pthread_mutex_lock(&info_mutex);
        read_next_seq_num = next_seq_num;
        pthread_mutex_unlock(&info_mutex);
        if (read_next_seq_num != ack_recv) {
            pthread_mutex_lock(&info_mutex);
	    read_cur_state = cur_state;
            pthread_mutex_unlock(&info_mutex);
	    printf_helper_recv_with_seq(read_cur_state, "Client receives incorrect ACK number from server\n", ack_recv, "DUMMY_TYPE");
        } else {
            pthread_mutex_lock(&info_mutex);
            last_type_recv = type_recv;
            cur_seq_num = ack_recv;
            pthread_mutex_unlock(&info_mutex);

            pthread_mutex_lock(&send_thread_sig_mutex);
            pthread_cond_signal(&send_thread_sig);
            pthread_mutex_unlock(&send_thread_sig_mutex);

            if (type_recv == FIN_ACK) {
                pthread_exit(NULL);
            }
        }
    }
}

/* Connect Function Call (mtcp Version) */
void mtcp_connect(int socket_fd, struct sockaddr_in *server_addr){
    struct thread_info args;
    args.socket_fd = socket_fd;
    args.server_addr = server_addr;

    STATE read_cur_state;
    unsigned char read_last_type_sent;
    pthread_mutex_lock(&info_mutex);
    read_cur_state = cur_state;
    pthread_mutex_unlock(&info_mutex);
    if (read_cur_state != INITIAL_STATE) {
        printf("already connected\n");
    } else {
        pthread_mutex_lock(&info_mutex);
        cur_state = THREE_WAY_HANDSHAKE_STATE;
        pthread_mutex_unlock(&info_mutex);

        if (pthread_create(&send_thread_pid, NULL, &send_thread, &args)) {
            perror("Fail to create sending thread\n");
            exit(-1);
        }
        if (pthread_create(&recv_thread_pid, NULL, &receive_thread, &args)) {
            perror("Fail to create receving thread\n");
            exit(-1);
        }

        printf_helper_app("THREE_WAY_HANDSHAKE_STATE", "wake sending thread to initiate THREE_WAY_HANDSHAKE");
        pthread_mutex_lock(&send_thread_sig_mutex);
        pthread_cond_signal(&send_thread_sig);
        pthread_mutex_unlock(&send_thread_sig_mutex);

        struct timespec timeToWait;
        struct timeval now;
        while(1) {
            gettimeofday(&now, NULL);
            timeToWait.tv_sec = now.tv_sec + 1;

            pthread_mutex_lock(&app_thread_sig_mutex);
            pthread_cond_timedwait(&app_thread_sig, &app_thread_sig_mutex, &timeToWait);
            pthread_mutex_unlock(&app_thread_sig_mutex);

            pthread_mutex_lock(&info_mutex);
            read_last_type_sent = last_type_sent;
            pthread_mutex_unlock(&info_mutex);
            if (read_last_type_sent == ACK) {
                break;
            }
        }
        pthread_mutex_lock(&info_mutex);
        cur_state = DATA_TRANSMISSION_STATE;
        pthread_mutex_unlock(&info_mutex);
        printf_helper_app("DATA_TRANSMISSION_STATE", "mtcp_connect return");
    }
    return;
}

/* Write Function Call (mtcp Version) */
int mtcp_write(int socket_fd, unsigned char *buf, int buf_len){
    // write buf to global_send_buf
    // wake sending thread up and return buf_len automatically
    STATE read_cur_state;                    // local version of global cur_state
    unsigned int read_local_send_buf_len;    // local version of local_send_buf_len
    int read_send_to_len;

    pthread_mutex_lock(&info_mutex);
    read_cur_state = cur_state;
    read_local_send_buf_len = local_send_buf_len;
    read_send_to_len = send_to_len;
    pthread_mutex_unlock(&info_mutex);


    if (read_send_to_len == -1) {
        return -1;
    }
    if (read_cur_state == FOUR_WAY_HANDSHAKE_STATE) {
        return 0;
    } else if (read_cur_state != DATA_TRANSMISSION_STATE) {
        return -1;
    }
    pthread_mutex_lock(&local_send_buf_mutex);
    memcpy(&(local_send_buf[read_local_send_buf_len]), buf, buf_len);
    pthread_mutex_unlock(&local_send_buf_mutex);

    pthread_mutex_lock(&info_mutex);
    local_send_buf_len += buf_len;
    pthread_mutex_unlock(&info_mutex);

    return buf_len;
}

/* Close Function Call (mtcp Version) */
void mtcp_close(int socket_fd){
    STATE read_cur_state;                    // local version of global cur_state
    unsigned char read_last_type_sent;
    unsigned char read_last_type_recv;

    pthread_mutex_lock(&info_mutex);
    read_cur_state = cur_state;
    pthread_mutex_unlock(&info_mutex);

    if (read_cur_state == DATA_TRANSMISSION_STATE) {
        struct timespec timeToWait;
        struct timeval now;
        pthread_mutex_lock(&info_mutex);
        cur_state = FOUR_WAY_HANDSHAKE_STATE;
        pthread_mutex_unlock(&info_mutex);
        while(1) {
            gettimeofday(&now, NULL);
            timeToWait.tv_sec = now.tv_sec + 1;

            pthread_mutex_lock(&app_thread_sig_mutex);
            pthread_cond_timedwait(&app_thread_sig, &send_thread_sig_mutex, &timeToWait);
            pthread_mutex_unlock(&app_thread_sig_mutex);

            pthread_mutex_lock(&info_mutex);
            read_last_type_sent = last_type_sent;
            read_last_type_recv = last_type_recv;
            pthread_mutex_unlock(&info_mutex);
            if ((read_last_type_sent == ACK) && (read_last_type_recv == FIN_ACK)) {
                break;
            }
        }
        pthread_mutex_lock(&info_mutex);
        cur_state = INITIAL_STATE;
        last_seq_num = -1;  
        cur_seq_num = 0;    
        next_seq_num = 0;   

        last_type_sent = INITIAL_MODE; 
        last_type_recv = INITIAL_MODE; 
        memset(&local_send_buf, 0, FILE_MAX_SIZE);
        local_send_buf_len = 0; 

        pthread_mutex_unlock(&info_mutex);
        close(socket_fd);
        printf_helper_app("INITIAL_STATE", "mtcp_close return");
    }
    return;
}
