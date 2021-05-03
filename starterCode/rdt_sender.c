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
#include <math.h>
#include <pthread.h>

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120 //milli second 
#define WINDOW_SIZE 10

long int maxAck = 0;

char buffer[DATA_SIZE];

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt = NULL;
sigset_t sigmask;       
int ackCtr = 0;
int portno, len;

FILE *fp;
long int file_size;

long int last_unacked;
long int last_sent;

float cwnd = 1.0;
long int in_flight_packets = 0;
int ssthresh = 64;

/*
 * This function send the last unacked packet if we get three duplicate acks 
 * or a timeout.
 */
void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        tcp_packet* sndpkt2;

        fseek(fp, maxAck, SEEK_SET);
        int len = fread(buffer, 1, DATA_SIZE, fp);

        sndpkt2 = make_packet(len);
        memcpy(sndpkt2->data, buffer, len);
        sndpkt2->hdr.seqno = maxAck;

        if(sendto(sockfd, sndpkt2, TCP_HDR_SIZE + get_data_size(sndpkt2), 0,
         ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
                error("sendto");
        }

        ssthresh = (cwnd/2 > 2 ? cwnd/2 : 2);
        cwnd = 1.0;
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

/*
 * Arguments to be passed in to the sending thread
 */
struct threadArgs {
    pthread_t previous_thread;
    FILE* read_ptr;
};

/*
 * Function to send packets - used as part of threading
 */
void *send_packets(void *input) {

    // This blocks the thread until the previous thread is done executing
    if (((struct threadArgs*) input)->previous_thread != NULL) {
        pthread_join(((struct threadArgs*) input)->previous_thread, NULL);
    }

    char sendBuffer[DATA_SIZE];

    int ctr = 0;
    int len = 0;

    tcp_packet* thread_send;

    while (last_sent < file_size && ctr < cwnd - in_flight_packets) {
        fseek(((struct threadArgs*) input)->read_ptr, last_sent, SEEK_SET);
        len = fread(sendBuffer, 1, DATA_SIZE, ((struct threadArgs*) input)->read_ptr);

        thread_send = make_packet(len);
        memcpy(thread_send->data, sendBuffer, len);
        thread_send->hdr.seqno = last_sent;

        if(sendto(sockfd, thread_send, TCP_HDR_SIZE + get_data_size(thread_send), 0, 
        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }

        last_sent += len;

        ctr++;
    }

    if (last_unacked > last_sent) in_flight_packets = 0;
    else in_flight_packets = ((last_sent - last_unacked) / DATA_SIZE) + 1;
}

int main (int argc, char **argv)
{
    char *hostname;

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
    
    init_timer(300, resend_packets);

    bool atEof = false;

    // In this part, we read a packet from the file and send it, to initialize
    // our sending window.

    len = fread(buffer, 1, DATA_SIZE, fp);

    if (len <= 0)
    {
        atEof = true;
    }

    sndpkt = make_packet(len);
    memcpy(sndpkt->data, buffer, len);
    sndpkt->hdr.seqno = 0;

    VLOG(DEBUG, "Sending packet %d to %s", 
         sndpkt->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
    if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
    {
        error("sendto");
    }

    last_sent = last_unacked = 0;

    // Making a file to save the cwnd and in flight packets to a csv
    FILE* cwnd_fp = fopen("cwnd_output.csv","w");
    setbuf(cwnd_fp, NULL);
    fprintf(cwnd_fp, "time_s,cwnd,in_flight_packets\n");

    pthread_t thread_id = NULL;

    while (1)
    {
        // Updating in flight packets
        if (last_unacked > last_sent) in_flight_packets = 0;
        else in_flight_packets = ((last_sent - last_unacked) / DATA_SIZE) + 1;

        printf("cwnd: %f; last_sent: %ld; last_unacked: %ld, ifp: %ld, fp at: %ld\n", cwnd, last_sent, last_unacked, in_flight_packets, ftell(fp));

        ftell(fp);

        // Here we make a thread to send the packets
        if (!atEof && cwnd-in_flight_packets > 0) {
            struct threadArgs *myArgs = (struct threadArgs *)malloc(sizeof(struct threadArgs));

            myArgs->previous_thread = thread_id;
            myArgs->read_ptr = fp;
            
            pthread_create(&thread_id, NULL, send_packets, myArgs);
        }

        if (last_sent == file_size) {
            atEof = true;
        }

        // If at end of file, we send an EOF packet, and wait to receive 
        // confirmation from the receiver.
        if (atEof && maxAck >= file_size) {

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

        long int receivedAck = 0;

        // This loop recieves acks from the receiver
        do {
            if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
            (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
            {
                error("recvfrom");
            }

            recvpkt = (tcp_packet *)buffer;

            assert(get_data_size(recvpkt) <= DATA_SIZE); 

            receivedAck = recvpkt->hdr.ackno;

            // Three duplicate acks
            if (receivedAck == maxAck) {
                ackCtr++;
                if (ackCtr == 3) {
                    stop_timer();
                    start_timer();
                    resend_packets(SIGALRM);
                    ackCtr = 0;
                }
            }
            if (receivedAck > maxAck) {
                maxAck = receivedAck;
                ackCtr = 0;
            } 

            // Break out if we get the ack we want
        } while (receivedAck <= last_unacked && receivedAck != file_size);

        ackCtr = 0;

        stop_timer();

        // A snippet to save cwnd and in flight packets
        long int temp_in_flight;
        if (last_unacked > last_sent) temp_in_flight = 0;
        else temp_in_flight = ((last_sent - last_unacked) / DATA_SIZE) + 1;
        fprintf(cwnd_fp, "%f,%f,%ld\n",(float)clock()/(float)CLOCKS_PER_SEC,cwnd,temp_in_flight);

        // Here we update cwnd depending on where it is in relation to ssthresh
        if (cwnd >= ssthresh) {
            cwnd += 1.0/(float)floor(cwnd);
        } else {
            cwnd += 1;
        }

        if (ceil(cwnd) - cwnd < 0.00001) cwnd = ceil(cwnd);

        last_unacked = maxAck;
    }

    fclose(fp);
    fclose(cwnd_fp);

    return 0;
}