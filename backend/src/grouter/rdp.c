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

// Global context for stop and wait
struct stopnwait_context *snw_context = NULL;

// Global context for the stop and wait timer
struct rdp_timer_context *snw_timer_context;

void rdp_init (int w_size,long timeout_length) {
	window_size = window_size;
	timeout = timeout_length;

	//Initialize stop and wait context
	snw_context = rdp_stopnwait_init();
	//Initialize the timer context for stop and wait
	snw_timer_context = rdp_timer_init(timeout_length, rdp_stopnwait_resend_packet, snw_context);
}

void rdp_shutdown() {
	//Shutdown the stopnwait service
	rdp_stopnwait_shutdown();
}

uint16_t add_ack_to_port(uint16_t port) {
	return port + 1;
}

uint16_t add_seq_num_to_port(uint16_t seq_num, uint16_t port) {
	//Shift by 1 since the first bit is reserved for the ack flag
	return port + (seq_num << 1);
}

uint16_t get_seq_num_from_rdp_port(uint16_t rdp_port) {
	//Since the seq nums start at the second bit, shift one bit down to get
	//actual sequence number
	return (rdp_port & RDP_SEQ_NUM_MASK) >> 1;
}

uint16_t get_ack_flag_from_rdp_port(uint16_t rdp_port) {
	return rdp_port & RDP_ACK_MASK;
}

uint16_t get_actual_port_from_rdp_port(uint16_t rdp_port) {
	return rdp_port - (get_seq_num_from_rdp_port(rdp_port)<<1) - get_ack_flag_from_rdp_port(rdp_port);
}

void print_pcb(struct udp_pcb *pcb) {
	printf("**********PCB************* \nlocal_address: ");
	for(int i = 0; i<4; i++) {
		printf("%x.", pcb->local_ip[i]);
	}
	printf("\n remote_address: ");
	for(int i = 0; i<4; i++) {
		printf("%x.", pcb->remote_ip[i]);
	}
	printf("\nlocal_port: %u \n remote_port: %u \n local_address: %u \n remote_address: %u \n********************** \n", pcb->local_port, pcb->remote_port);
}


/*****************************
RDP STOP AND WAIT INTERFACE
*****************************/

struct stopnwait_context* rdp_stopnwait_init() {
	// Allocate space for the new context
	struct stopnwait_context *context = NULL;
	context = (struct stopnwait_context *) malloc(sizeof(struct stopnwait_context));
	if(context == NULL) {
		printf("Error initializing the stop and wait context.\n");
	}

	//Initialize the new context's values
	context->waiting = 0;
	context->next_seq_num = 0;
	context->seq_num_expected_to_recv = 0;
	context->pcb = NULL;
	context->payload = NULL;

	return context;
}

err_t
rdp_stopnwait_send(struct udp_pcb *pcb, struct pbuf *p)
{
	//For now if the current port being used does not have a valid rdp port format (bits captured by mask should be 0)
	//then hardcode the local port to be 5012
	int not_valid_rdp_port = pcb->local_port & RDP_VALID_RDP_PORT_MASK;
	if(not_valid_rdp_port){
		pcb->local_port = 5012;
	}

	//Make sure to only proceed if we're not already waiting for an acknowledgement
	if(snw_context->waiting) {
		//If not then we refuse data
		printf("Still waiting for acknowledgements for message %s", (char *)snw_context->payload);
		return ERR_OK;
	}

	//Get the next sequence number to use 
	int next_seq_num = snw_context -> next_seq_num;
	//Update the next available sequence number in the context
	snw_context->next_seq_num = (next_seq_num + 1)%2;

	//Save the actual local port to reset pcb after all is done
	uint16_t actual_port = pcb->local_port;

	//Encode the sequence number into the local port
	pcb->local_port = add_seq_num_to_port(next_seq_num, actual_port);

	//Set waiting for ack in context
	snw_context->waiting = 1;

	//Store the pcb in the context in case we need to resend
	snw_context->pcb = pcb;

	//Make sure to free if there was already a payload in the context from before
	if(snw_context->payload != NULL) {
		free(snw_context->payload);
		snw_context->payload = NULL;
	}

	//Allocate space in the context to remember the payload in case we need to resend it
	//Note that the +1 is important to allow for null termination
	snw_context->payload = (char *) malloc(strlen(p->payload) +1);
	//Copy the current payload into the context
	strcpy(snw_context->payload, p->payload);

	//Send to udp
	err_t err = udp_send(pcb, p);

	//set timer
	rdp_timer_start(snw_timer_context);

	//Reset the pcb so that we have the proper port for next time
	pcb -> local_port = actual_port;

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
	//retrieve sequence number
	uint16_t seq_num = get_seq_num_from_rdp_port(port);

	//Is it an ack packet
	int is_ack_packet = get_ack_flag_from_rdp_port(port);

	//Retrieve the actual port from the port received
	uint16_t actual_port = get_actual_port_from_rdp_port(port);

    uchar ipaddr_network_order[4];
    gHtonl(ipaddr_network_order, addr);
    udp_connect(arg, ipaddr_network_order, actual_port);

    //The sequence number we are waiting on after having sent it
    uint16_t seq_num_expected = abs(snw_context->next_seq_num -1)%2;
    if(is_ack_packet) {
    	//If it is an ack packet and for the sequence number we are waiting for then we update the context
    	if(seq_num == seq_num_expected){
    		if(snw_context->waiting) {
    			printf("Got acknowledgement.\n");
    		}
    		snw_context->waiting = 0;
    		
    		//Reset the timer
    		rdp_timer_reset(snw_timer_context);
    	}

    	//if an ack we weren't expecting then we do nothing
    }

    //Otherwise we got a proper packet from someone else and should send an acknowledgement
    else {
    	//The packet has the next sequence number we were expecting
    	if(seq_num == snw_context->seq_num_expected_to_recv) {
    		//Update the sequence number we expect to receive after this
    		snw_context->seq_num_expected_to_recv = (seq_num + 1)%2;

    		//Deliver the packet
    		printf("%s", (char *)p->payload);
    	}

    	//Here we send back an ack packet to sender

    	//Store our local port
    	uint16_t local_port = pcb -> local_port;
    	//add both an ack flag as well as the seq number to the port and make that our sending port
    	pcb->local_port = add_seq_num_to_port(seq_num, add_ack_to_port(local_port));

    	//Allocate new pbuf and set payload
    	char *payload = "ack!!!";
    	struct pbuf *ack_p = pbuf_alloc(PBUF_TRANSPORT, strlen(payload), PBUF_RAM);
    	ack_p->payload = payload;

    	//Send the acknowledgement back to the sender
    	udp_send(pcb, ack_p);

    	//Reset the local port to be our original one, so that it's clean for the next time.
    	pcb->local_port = local_port;
    }
}

void rdp_stopnwait_resend_packet(void *arg) {
	struct stopnwait_context *context = (struct stopnwait_context *) arg;

	//Retrive the pcb representing the connection from the context
	struct udp_pcb *pcb = context->pcb;
	//Allocate a new pbuffer in which to resend the message
	struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, strlen(context->payload), PBUF_RAM);

	//copy the payload into a new buffer
	char payload[DEFAULT_MTU];
	strcpy(payload, context->payload);
	//Set the pbuf's payload
    p->payload = payload;

    //Keep local port to be able to sanitize pcb after all of this
	uint16_t actual_port = pcb->local_port;
	//Add the sequence number that was last send to the port
	pcb->local_port = add_seq_num_to_port(abs(snw_context->next_seq_num -1)%2, actual_port);

	//Resend the packet
	udp_send(pcb, p);

	//Reset the pcb's local port to the actual port without the sequence number.
	pcb->local_port = actual_port;
}

void rdp_stopnwait_shutdown() {

	if(snw_context != NULL) {
		if(snw_context->payload != NULL) {
			free(snw_context->payload);
		}

		free(snw_context);
		snw_context = NULL;
	}
	

	rdp_timer_shutdown(snw_timer_context);
}

