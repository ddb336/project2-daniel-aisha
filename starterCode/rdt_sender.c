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

char buffer[DATA_SIZE];

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       
tcp_packet* window[WINDOW_SIZE] = {NULL};
int packetsInWindow = 0;
int ackCtr = 0;

FILE *fp;
long int file_size;

long int last_unacked;
long int last_sent;

long int cwnd = 1;

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
     //    if (last_unacked->hdr.ctr_flags != END) VLOG(INFO, "Resending packets.");

     //    // VLOG(DEBUG, "Retry sending packet %d to %s", 
     //    //         last_unacked->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
     //    if (last_unacked != NULL) {
     //        printf("Resending: %d\n",last_unacked->hdr.seqno);
		   //  if(sendto(sockfd, last_unacked, TCP_HDR_SIZE + get_data_size(last_unacked), 0,
     //   		 ( const struct sockaddr *)&serveraddr, serverlen) < 0)
     //    	{
     //        		error("sendto");
     //    	}
	    // }

        fseek(fp, last_unacked, SEEK_SET);
        int len = fread(buffer, 1, DATA_SIZE, fp);

        sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        sndpkt->hdr.seqno = last_unacked;

        printf("Resending: %d\n",sndpkt->hdr.seqno);
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0,
         ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
                error("sendto");
        }


        // for (size_t i = (last_unacked_idx+1)%WINDOW_SIZE; i != (last_sent_idx+1)%WINDOW_SIZE; i = (i+1) % WINDOW_SIZE)
        // {
        //     if (window[i] == NULL) continue;

        //     VLOG(DEBUG, "Retry sending packet %d to %s\n", 
        //         window[i]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
        //     if(sendto(sockfd, window[i], TCP_HDR_SIZE + get_data_size(window[i]), 0, 
        //     ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        //     {
        //         error("sendto");
        //     }
        // }
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

int main (int argc, char **argv)
{
    int portno, len, seek;
    char *hostname;

    /* check command line arguments */
    if (argc != 4) {
        //fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    fseek(fp, 0L, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0L, SEEK_SET);

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

    // ------------------------------------------------ //

    len = fread(buffer, 1, DATA_SIZE, fp);

    if (len <= 0)
    {
        atEof = true;
    }

    sndpkt = make_packet(len);
    memcpy(sndpkt->data, buffer, len);
    sndpkt->hdr.seqno = next_seqno;

    next_seqno = next_seqno + len;

    VLOG(DEBUG, "Sending packet %d to %s", 
         sndpkt->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
    if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
    {
        error("sendto");
    }

    last_sent = last_unacked = 0;

   // ------------------------------------------------ //

    while (1)
    {

        printf("Last sent: %ld; last unacked: %ld;\n",last_sent,last_unacked);

        long int in_flight_packets;

        if (last_unacked > last_sent) in_flight_packets = 0;
        else in_flight_packets = ((last_sent - last_unacked) / DATA_SIZE) + 1;

        printf("Ifp: %ld;\n",in_flight_packets);

        printf("cwnd: %ld\n", cwnd);

        int pid = fork();

        if (pid == 0) 
        {
            while (in_flight_packets < cwnd) 
            {
                in_flight_packets++;
                
                last_sent += DATA_SIZE;

                seek = fseek(fp, last_sent, SEEK_SET);

                if (seek != 0)
                {
                    printf("seek error!\n");
                }

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

                VLOG(DEBUG, "Sending packet %d to %s\n", 
                     sndpkt->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
                if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                ( const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }
            }

            return 0;
        }

        int ctr = 0;

        while (len > 0 && ctr < cwnd - in_flight_packets) {
            last_sent += DATA_SIZE;
            next_seqno += DATA_SIZE; 
            ctr++;
            seek = fseek(fp, last_sent, SEEK_SET);
            len = fread(buffer, 1, DATA_SIZE, fp);
        }

        if (len <= 0) atEof = true;

        if (atEof && maxAck >= file_size) {

            printf("entered eof\n");
            printf("next_seqno: %d\n",next_seqno);

            sndpkt = make_packet(0);
            sndpkt->hdr.ctr_flags = END;
            sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                    (const struct sockaddr *)&serveraddr, serverlen);

            last_unacked = sndpkt->hdr.seqno;

            int num_tries = 0;

            start_timer();

            do {
                if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
                {
                    error("recvfrom");
                }

                recvpkt = (tcp_packet *)buffer;
                assert(get_data_size(recvpkt) <= DATA_SIZE);

                num_tries++;
            } while (recvpkt->hdr.ctr_flags != END);

            stop_timer();
            VLOG(INFO, "FIN received. End Of File has been reached");
            break;
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

            printf("Got ACK: %d\n", recvpkt->hdr.ackno);

            if (recvpkt->hdr.ackno == maxAck) {
                ackCtr++;
                if (ackCtr == 3) {
                    stop_timer();
                    printf("got 3 duplicates\n");
                    resend_packets(SIGALRM);
                    ackCtr = 0;
                    start_timer();
                }
            }

            if (recvpkt->hdr.ackno > maxAck) {
                maxAck = recvpkt->hdr.ackno;
                ackCtr = 0;
            }

        } while (recvpkt->hdr.ackno < last_unacked);

        stop_timer();


        int space_between = maxAck - last_unacked;

        cwnd += space_between/DATA_SIZE;

        last_unacked = maxAck;
    }

    fclose(fp);

    return 0;
}




