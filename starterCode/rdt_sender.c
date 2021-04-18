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
#define WINDOW_SIZE 10

int next_seqno = 0;
int send_base = 0;
int maxAck = 0;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       
tcp_packet* window[WINDOW_SIZE] = {NULL};
int packetsInWindow = 0;


void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timout happend");

        for (size_t i = 0; i < packetsInWindow; i++)
        {
            if (window[i]->hdr.seqno == maxAck) {
                printf("Timeout sending packet %d\n",window[i]->hdr.seqno);
                if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                (const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }
            }
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


int main (int argc, char **argv)
{
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
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

    init_timer(RETRY, resend_packets);
    next_seqno = 0;
    bool atEof = false;

    while (1)
    {
        send_base = next_seqno;

        for (size_t i = 0; i < WINDOW_SIZE; i++)
        {
            window[i] = NULL;
        }

        packetsInWindow = WINDOW_SIZE;
        for (size_t i = 0; i < WINDOW_SIZE; i++)
        {
            len = fread(buffer, 1, DATA_SIZE, fp);

            if (len <= 0)
            {
                atEof = true;
                packetsInWindow = i;
                break;
            }
            
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = next_seqno;

            next_seqno = next_seqno + len;

            window[i] = sndpkt;
        }

        //Wait for ACK
        maxAck = -1;
        do {
            /*
             * If the sendto is called for the first time, the system will
             * will assign a random port number so that server can send its
             * response to the src port.
             */
            for (size_t i = 0; i < packetsInWindow; i++)
            {
                if (window[i]->hdr.seqno > maxAck) {
                    VLOG(DEBUG, "Sending packet %d to %s", 
                    window[i]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
                    if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
                    {
                        error("sendto");
                    }
                }
            }

            start_timer();

            //ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
            //struct sockaddr *src_addr, socklen_t *addrlen);
            for (size_t i = 0; i < packetsInWindow; i++)
            {
                if (window[i]->hdr.seqno >= maxAck) {
                    if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                                (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
                    {
                        error("recvfrom");
                    }
                    recvpkt = (tcp_packet *)buffer;
                    
                    assert(get_data_size(recvpkt) <= DATA_SIZE);

                    if (recvpkt->hdr.ackno > maxAck) {
                        maxAck = recvpkt->hdr.ackno;
                    }
                }
            }
            
            stop_timer();
            /* resend pack if dont recv ack */

            printf("next_seqno: %d, maxack: %d, packetsInWindow: %d\n", next_seqno, maxAck, packetsInWindow);
        } while (maxAck != next_seqno && packetsInWindow > 0);

        for (size_t i = 0; i < WINDOW_SIZE; i++)
        {
            free(window[i]);
            window[i] = NULL;
        }

        if (atEof) {
            VLOG(INFO, "End Of File has been reached");
            sndpkt = make_packet(0);
            sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                    (const struct sockaddr *)&serveraddr, serverlen);
            break;
        }
    }

    return 0;

}



