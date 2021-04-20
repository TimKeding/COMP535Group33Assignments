#include <stdlib.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/timeb.h> 
#include "grouter.h"
#include "ip.h"
#include "udp.h"
#include "err.h"
#include "debug.h"
#include "memp.h"
#include "protocols.h"
#include "opt.h"
#include "rdp.h"
#include "rdp_timer.h"


int window_size=0;
long timeout;

struct stopnwait_context *snw_context = NULL;
struct rdp_timer_context *snw_timer_context;

void rdp_init (int w_size,long timeout_length) {
	window_size = window_size;
	timeout = timeout_length;

	//Initialize stop and wait context
	snw_context = rdp_stopnwait_init();
	

	snw_timer_context = rdp_timer_init(timeout_length, rdp_stopnwait_resend_packet, snw_context);
}

void rdp_shutdown() {
	free(snw_context);
	snw_context = NULL;

	rdp_timer_shutdown(snw_timer_context);
}

uint16_t add_ack_to_port(uint16_t port) {
	return port + 1;
}

uint16_t add_seq_num_to_port(uint16_t seq_num, uint16_t port) {
	return port + (seq_num << 1);	//Shift by 1 since the first bit is reserved for the ack flag
}

uint16_t get_seq_num_from_rdp_port(uint16_t rdp_port) {
	return (rdp_port & RDP_SEQ_NUM_MASK) >> 1;
}

uint16_t get_ack_flag_from_rdp_port(uint16_t rdp_port) {
	return rdp_port & RDP_ACK_MASK;
}

uint16_t get_actual_port_from_rdp_port(uint16_t rdp_port) {
	return rdp_port - (get_seq_num_from_rdp_port(rdp_port)<<1) - get_ack_flag_from_rdp_port(rdp_port);
}

void print_pcb(struct udp_pcb *pcb) {
	printf("**********PCB*************\n local_port: %u \n remote_port: %u \n ********************** \n", pcb->local_port, pcb->remote_port);
}


/*****************************
RDP STOP AND WAIT INTERFACE
*****************************/

struct stopnwait_context* rdp_stopnwait_init() {
	printf("Initializing the snw contexta\n");
	struct stopnwait_context *context = NULL;

	context = (struct stopnwait_context *) malloc(sizeof(struct stopnwait_context));
	if(context == NULL) {
		printf("Error initializing the stop and wait context.\n");
	}

	context->waiting = 0;
	context->next_seq_num = 0;
	context->seq_num_expected_to_recv = 0;

	return context;
}

err_t
rdp_stopnwait_send(struct udp_pcb *pcb, struct pbuf *p)
{
	printf("\n\n%s\n", "Inside send!");
	// print_pcb(pcb);
	// printf("CONTEXT: waiting: %d, next seq: %u \n", snw_context->waiting, snw_context->next_seq_num);

	//For now if the current port being used does not have a valid rdp port format (bits captured by mask should be 0)
	//then hardcode the local port to be 5012
	int not_valid_rdp_port = pcb->local_port & RDP_VALID_RDP_PORT_MASK;
	// printf("Not valid port already? %d\n", not_valid_rdp_port);
	if(not_valid_rdp_port){
		pcb->local_port = 5012;
	}
	// printf("After setting local port\n"); print_pcb(pcb);

	//Check if space in window
	if(snw_context->waiting) {
		//If not then we refuse data
		printf("%s\n", "Waiting for acknowledgements try again later.");
		return ERR_OK;
	}
	// printf("%s\n", "Not waiting! Can send the tea");

	//Set sequence number. 
	int next_seq_num = snw_context -> next_seq_num;
	//Save the actual local port to reset pcb after all is done
	uint16_t actual_port = pcb->local_port;
	pcb->local_port = add_seq_num_to_port(next_seq_num, actual_port);
	// printf("After setting local port in send\n"); print_pcb(pcb);

	//Update the next available sequence number in the context
	snw_context->next_seq_num = (next_seq_num + 1)%2;
	// printf("After updating the seq num, the context says next seq num is %u\n", (unsigned int)snw_context->next_seq_num);

	//Set payload checksum

	//Set waiting for ack in context
	snw_context->waiting = 1;

	//Store the pcb and p in the context
	snw_context->pcb = pcb;
	snw_context->payload = (char *)p->payload;
	// printf("Stored pbuf p payload: %s\n", (char*) snw_context->payload);

	//Send to udp
	err_t err = udp_send(pcb, p);
	// printf("Sent with seq number %u\n", next_seq_num);

	//set timer
	rdp_timer_start(snw_timer_context);

	//Reset the pcb so that we have the proper port for next time
	pcb -> local_port = actual_port;

	// printf("After resetting remote port so that it does not affect next time\n"); print_pcb(pcb);

	return err;
}

void rdp_stopnwait_recv_callback (
	void *arg, 
	struct udp_pcb *pcb, 
	struct pbuf *p, 
	uchar *addr, 
	uint16_t port
)
{
	printf("\nIn stop and wait receive\n");
	// print_pcb(pcb);
	printf("DEBUG: %s\n", (char *)p->payload);

	//retrieve sequence number
	uint16_t seq_num = get_seq_num_from_rdp_port(port);
	// printf("Received sequence number: %d, expected: %d\n", seq_num, abs(snw_context->next_seq_num -1)%2);

	int is_ack_packet = get_ack_flag_from_rdp_port(port);
	// printf("Received ack packet? %d\n", is_ack_packet);

	uint16_t actual_port = get_actual_port_from_rdp_port(port);
	// printf("Received port: %u, Actual port: %u\n", port, actual_port);

    uchar ipaddr_network_order[4];
    gHtonl(ipaddr_network_order, addr);
    udp_connect(arg, ipaddr_network_order, actual_port);

    //If it is an ack packet and for the sequence number we are waiting for then we update the context
    uint16_t seq_num_expected = abs(snw_context->next_seq_num -1)%2;
    if(is_ack_packet) {
    	printf("Previous sequence number that was sent was %u\n", seq_num_expected);

    	if(seq_num == seq_num_expected){
    		printf("Got ack for sequence number: %d\n", seq_num);
    		snw_context->waiting = 0;
    		//Reset the timer
    		rdp_timer_reset(snw_timer_context);
    	}

    	//if an ack we weren't expecting then we do nothing
    } 
    //Otherwise we got a proper packet from someone else and should send an acknowledgement
    else {
    	printf("Expected to receive sequence number %u, and received sequence number %u\n", snw_context->seq_num_expected_to_recv, seq_num);

    	if(seq_num == snw_context->seq_num_expected_to_recv) {
    		//Got a correctly ordered packet
    		printf("Entering odinsleep\n");
    		sleep(5);
    		printf("I am awkake!\n");
    		//Update the sequence number we expect to receive after this
    		snw_context->seq_num_expected_to_recv = (seq_num + 1)%2;
    		// printf("next sequenece number expected to be received is now %u\n", snw_context->seq_num_expected_to_recv);
    		//Deliver the packet
    		printf("RDP: %s\n", (char *)p->payload);
    	} 
    	else{
    		printf("Already delivered seq num %u\n", seq_num);
    	}

    	//send back an ack packet to sender
    	uint16_t local_port = pcb -> local_port;
    	pcb->local_port = add_seq_num_to_port(seq_num, add_ack_to_port(local_port));
    	print_pcb(pcb);
    	udp_send(pcb, p);
    	pcb->local_port = local_port;
    	printf("Done sending ack back\n");
    }
}

void rdp_stopnwait_resend_packet(void *arg) {
	struct stopnwait_context *context = (struct stopnwait_context *) arg;

	struct udp_pcb *pcb = context->pcb;
	struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, strlen(context->payload), PBUF_RAM);
    p->payload = context->payload;
	// printf("In resend, the payload is %s %s\n", (char *)context->payload, (char*)p->payload);

	uint16_t actual_port = pcb->local_port;
	pcb->local_port = add_seq_num_to_port(abs(snw_context->next_seq_num -1)%2, actual_port);
	printf("Sending packet again!\n");
	print_pcb(pcb);
	udp_send(pcb, p);

	//Reset the pcb's local port to the actual port without the sequence number.
	pcb->local_port = actual_port;
}

/*****************************/