#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <stdbool.h>

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120 //milli second 
#define WINDOW_SIZE 50

int next_seqno = 0;
int send_base = 0;
int maxAck = 0;

char buffer[DATA_SIZE];

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       
tcp_packet* window[WINDOW_SIZE] = {NULL};
int packetsInWindow = 0;

tcp_packet* last_unacked;
int last_unacked_idx;

tcp_packet* last_sent;
int last_sent_idx;

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timout happend");

        VLOG(DEBUG, "Sending packet %d to %s", 
        last_unacked->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
        if(sendto(sockfd, last_unacked, TCP_HDR_SIZE + get_data_size(last_unacked), 0, 
        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
    }
}


void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}


void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

/*
 * init_timer: Initialize timeer
 * delay: delay in milli seconds
 * sig_handler: signal handler function for resending unacknoledge packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, resend_packets);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

int space_left(int last_unacked_idx, int last_sent_idx) {
    if (last_unacked_idx > last_sent_idx) {
        return last_unacked_idx - last_sent_idx - 1;
    } else {
        return WINDOW_SIZE - (last_sent_idx - last_unacked_idx + 1);
    }
}

void print_window() {
    for (size_t i = 0; i < WINDOW_SIZE; i++)
    {
        printf("[");
        if (window[i] == NULL) {
            printf("NULL");
        } else {
            printf("%d",window[i]->hdr.seqno);
        }
        printf("]");
    }
    printf("\n");
}

int main (int argc, char **argv)
{
    int portno, len;
    char *hostname;
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //Stop and wait protocol

    //RETRY
    init_timer(300, resend_packets);
    next_seqno = 0;
    bool atEof = false;

    // Fill the window & send all
    for (size_t i = 0; i < WINDOW_SIZE; i++)
    {
        len = fread(buffer, 1, DATA_SIZE, fp);

        if (len <= 0)
        {
            atEof = true;
            break;
        }

        sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        sndpkt->hdr.seqno = next_seqno;

        next_seqno = next_seqno + len;

        window[i] = sndpkt;

        VLOG(DEBUG, "Sending packet %d to %s", 
           sndpkt->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
    }

    // Initialize the pointers & indices
    last_unacked = window[0];
    last_unacked_idx = 0;

    last_sent = window[WINDOW_SIZE - 1];
    last_sent_idx = WINDOW_SIZE - 1;

    while (1)
    {
        // If there's any space in our window, fill it up with packets
        if (space_left(last_unacked_idx, last_sent_idx) > 0) {
            for (size_t i = last_sent_idx; i < WINDOW_SIZE; i++)
            {
                if (window[i] == NULL) {
                    len = fread(buffer, 1, DATA_SIZE, fp);

                    if (len <= 0)
                    {
                        atEof = true;
                        break;
                    }
                    
                    sndpkt = make_packet(len);
                    memcpy(sndpkt->data, buffer, len);
                    sndpkt->hdr.seqno = next_seqno;

                    next_seqno = next_seqno + len;
                    
                    window[i] = sndpkt;
                }
            }

            for (size_t i = 0; i < last_unacked_idx; i++)
            {
                if (window[i] == NULL) {
                    len = fread(buffer, 1, DATA_SIZE, fp);

                    if (len <= 0)
                    {
                        atEof = true;
                        break;
                    }
                    
                    sndpkt = make_packet(len);
                    memcpy(sndpkt->data, buffer, len);
                    sndpkt->hdr.seqno = next_seqno;

                    next_seqno = next_seqno + len;
                    
                    window[i] = sndpkt;
                }
            }

            //print_window();

            // Fork to send all unsent packets and receive acks
            int pid = fork();

            if (pid) {

                //printf(" (last_sent_idx + 1)mod WINDOW_SIZE: %d ; last_unacked_idx: %d \n", (last_sent_idx + 1) % WINDOW_SIZE, last_unacked_idx);
                for (size_t i = (last_sent_idx + 1) % WINDOW_SIZE; i != last_unacked_idx; i = (i+1) % WINDOW_SIZE)
                {

                    //window[last_unacked_idx-1]->hdr.seqno < last_sent->hdr.seqno
                    if (window[i] == NULL) continue;
                    VLOG(DEBUG, "Sending packet %d to %s", 
                    window[i]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
                    if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
                    {
                        error("sendto");
                    }
                }

                if (atEof) {
                    sndpkt = make_packet(0);
                    sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                            (const struct sockaddr *)&serveraddr, serverlen);
                }
                
                kill(pid, SIGKILL);
            }

            last_sent_idx = ((last_unacked_idx-1) + WINDOW_SIZE) % WINDOW_SIZE;
            last_sent = window[last_sent_idx];
        }

        start_timer();

        do {

            if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
            (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
            {
                error("recvfrom");
            }

            recvpkt = (tcp_packet *)buffer;
            assert(get_data_size(recvpkt) <= DATA_SIZE);   

            if (recvpkt->hdr.ackno > maxAck) maxAck = recvpkt->hdr.ackno;

        } while (recvpkt->hdr.ackno < last_unacked->hdr.seqno);

        stop_timer();

        // printf("maxAck: %d; next_seqno: %d;\n",maxAck,next_seqno);
        if (atEof && maxAck == next_seqno) {
            VLOG(INFO, "End Of File has been reached");
            break;
        }

        for (size_t i = 0; i < WINDOW_SIZE; i++)
        {
            if (window[i] == NULL) continue;
            if (window[i]->hdr.seqno == recvpkt->hdr.ackno) {
                last_unacked_idx = i;
                last_unacked = window[i];
            }
            if (window[i]->hdr.seqno < recvpkt->hdr.ackno) {
                free(window[i]);
                window[i] = NULL;
            }
        }
    }

    return 0;

}




