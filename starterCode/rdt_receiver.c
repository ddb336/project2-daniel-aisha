#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>
#include <stdbool.h>

#include "common.h"
#include "packet.h"

// Max. Bandwidth = 30 Mb/s
// RTT = 10 ms 
// Buffer = 30*10^6 * 10*10^-3 * 2 for safety = 600,000
#define RECV_BUFFER_SIZE (600000/DATA_SIZE)

void print_buffer(tcp_packet* recv_buffer[]) {
    for (size_t i = 0; i < RECV_BUFFER_SIZE; i++)
    {
        printf("[%zu: ", i);
        if (recv_buffer[i] == NULL) {
            printf("NULL");
        } else {
            printf("%d",recv_buffer[i]->hdr.seqno);
        }
        printf("]");
    }
    printf("\n");
}

/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the currenlt implemenetation window size is one, hence we have
 * onlyt one send and receive packet
 */
tcp_packet *recvpkt;
tcp_packet *sndpkt;

int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];

    int numpacks = 0;

    struct timeval tp;

    tcp_packet* recv_buffer[RECV_BUFFER_SIZE] = {NULL};

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);

    printf("Waiting for files\n");

    int exp_seqno = 0;
    int recv_base_idx = RECV_BUFFER_SIZE - 1;

    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */

        // printf("0\n");
        // print_buffer(recv_buffer);
        // printf("recv base idx: %d\n", recv_base_idx);

        //VLOG(DEBUG, "waiting from server \n");


        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom\n");
        }

        numpacks++;

        recvpkt = (tcp_packet *) buffer;

        assert(get_data_size(recvpkt) <= DATA_SIZE);

        if (recvpkt->hdr.data_size == 0) {
            VLOG(INFO, "End Of File has been reached");

            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = exp_seqno;
            sndpkt->hdr.ctr_flags = END;

            printf("sndpkt->hdr.ackno: %d\n",sndpkt->hdr.ackno);

            for (size_t i = 0; i < 10; i++)
            {
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            }

            printf("Numpacks: %d",numpacks);


            fclose(fp);
            close(sockfd);
            break;
        }

        if (recvpkt->hdr.seqno < exp_seqno && recvpkt->hdr.seqno % DATA_SIZE == 0) {
            printf("less than exp_seqno: %d\n", recvpkt->hdr.seqno);

            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = exp_seqno;
            sndpkt->hdr.ctr_flags = ACK;

            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }

            continue;
        }

        if (recvpkt->hdr.seqno > exp_seqno) {

            printf("greater than exp seq number: %d, sending ack for %d\n", recvpkt->hdr.seqno, exp_seqno);

            if (recvpkt->hdr.seqno <= exp_seqno + DATA_SIZE*(RECV_BUFFER_SIZE - 1)) {

                printf("Writing to buffer: %d\n",recvpkt->hdr.seqno);

                int pack_idx = (((recvpkt->hdr.seqno - exp_seqno) / DATA_SIZE) + recv_base_idx) % RECV_BUFFER_SIZE;

                recv_buffer[pack_idx] = (tcp_packet*)malloc(TCP_HDR_SIZE + recvpkt->hdr.data_size); 
                memcpy(recv_buffer[pack_idx]->data, recvpkt->data, recvpkt->hdr.data_size);
                recv_buffer[pack_idx]->hdr = recvpkt->hdr;
            }

            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = exp_seqno;
            sndpkt->hdr.ctr_flags = ACK;

            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            continue;
        }

        /*
         * sendto: ACK back to the client 
        //  */
        // gettimeofday(&tp, NULL);
        // VLOG(DEBUG, "%lu, %d, %d\n", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

        // printf("Ftell at: %ld\n", ftell(fp));
        // printf("Received: %d\n",recvpkt->hdr.seqno);
        // fseek(fp, recvpkt->hdr.seqno, SEEK_SET);

        // printf("Ftell2 at: %ld\n", ftell(fp));
        // printf("writing: %d\n", recvpkt->hdr.seqno);
        // printf("data size: %d\n", recvpkt->hdr.data_size);
        fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);

        // printf("1\n");
        // print_buffer(recv_buffer);

        tcp_packet* to_write = recv_buffer[(recv_base_idx+1)%RECV_BUFFER_SIZE];

        int last_written_seqno = recvpkt->hdr.seqno;
        int last_written_data_size = recvpkt->hdr.data_size;

        bool entered_loop = false;

        while (to_write != NULL) 
        {
            fseek(fp, to_write->hdr.seqno, SEEK_SET);
            printf("buffer ftell at: %ld\n",ftell(fp));
            printf("writing from buffer: %d\n",to_write->hdr.seqno);
            fwrite(to_write->data, 1, to_write->hdr.data_size, fp);

            last_written_seqno = to_write->hdr.seqno;
            last_written_data_size = to_write->hdr.data_size;

            free(recv_buffer[(recv_base_idx+1)%RECV_BUFFER_SIZE]);

            recv_buffer[(recv_base_idx+1)%RECV_BUFFER_SIZE] = NULL;

            recv_base_idx = (recv_base_idx+1)%RECV_BUFFER_SIZE;

            to_write = recv_buffer[(recv_base_idx+1)%RECV_BUFFER_SIZE];

            entered_loop = true;
        }

        if (entered_loop) recv_base_idx = (recv_base_idx+1)%RECV_BUFFER_SIZE;

        sndpkt = make_packet(0);
        exp_seqno = last_written_seqno + last_written_data_size;
        sndpkt->hdr.ackno = exp_seqno;
        sndpkt->hdr.ctr_flags = ACK;

        // printf("expecting: %d\n",exp_seqno);

        printf("Acking: %d\n", sndpkt->hdr.ackno);
        // printf("2\n");
        // print_buffer(recv_buffer);

        // if (exp_seqno > 10000 && !(numpacks%2 == 0)) {
        //     continue;
        // }

        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
            error("ERROR in sendto");
        }
    }

    return 0;
}
