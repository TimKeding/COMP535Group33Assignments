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


int window_size = 0;
long timeout;
int old_seq_num = 0;

// Global context for stop and wait
struct stopnwait_context *snw_context = NULL;

// Global context for the stop and wait timer
struct rdp_timer_context *snw_timer_context;

// Global context for go-back-n
struct gobackn_context *gbn_context = NULL;

// Global context for the go-back-n timer
struct rdp_timer_context *gbn_timer_context;

void rdp_init (int w_size,long timeout_length) {
	window_size = w_size;
	timeout = timeout_length;

	//Initialize stop and wait context
	snw_context = rdp_stopnwait_init(); 
	//Initialize the timer context for stop and wait
	snw_timer_context = rdp_timer_init(timeout_length, rdp_stopnwait_resend_packet, snw_context);

	//Initialize stop and wait context
	gbn_context = rdp_gobackn_init(); //changing temporarily !!!!
	//Initialize the timer context for stop and wait
	gbn_timer_context = rdp_timer_init(timeout_length, rdp_gobackn_resend_packet, gbn_context);
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

/*****************************
RDP GO-BACK-N INTERFACE
*****************************/

struct gobackn_context*	rdp_gobackn_init (){
// Allocate space for the new context
	struct gobackn_context *context = NULL;
	context = (struct gobackn_context *) malloc(sizeof(struct gobackn_context));
	if(context == NULL) {
		printf("Error initializing the stop and wait context.\n");
	}
	printf("currently initializing gobackn...\n");
	//Initialize the new context's values

	context->send_base = 0;					
	context->next_seq_num = 0;				
	context->waiting = 0;						
	//context->pcb[MAX_N_CALLBACK] = NULL;
	context->num_pcb_stored = 0;
	context->seq_start = 0;
	//context->payload[MAX_N_CALLBACK] = NULL;						
	context->seq_num_expected_to_recv = 0;

	return context;
}

void rdp_gobackn_recv_callback (
	void *arg, 
	struct udp_pcb *pcb, 
	struct pbuf *p, 
	uchar *addr, 
	uint16_t port)
{
	printf("in callback\n");
	//cannot keep the sequence number as an argument, hide it in the port
	//since also using ack mask,

	//have latest sequence number received as global var old_seq_num 
	printf("port: %d\n", port); //this gives me the modified port with the seq num 5012
	printf("pcb port: %d\n", pcb->local_port); //5000
	//Is it an ack packet -- can still use this for go-back-n
	int is_ack_packet = get_ack_flag_from_rdp_port(port);
	printf("is it an ack packet? %d \n", is_ack_packet); //1 meaning yes
	printf("12/10 hoping it gives 1, then would take away 1 to give 0");
	printf("%d\n", ((5012-5000)/10)-1);

	//Retrieve the sequence number of the packet:
	uint16_t sequence_number = ((pcb->local_port - port)/10) - 1;

	//Retrieve actual port:
	uint16_t actual_port = port - seq_num*10;

	uchar ipaddr_network_order[4];
	gHtonl(ipaddr_network_order, addr);
	udp_connect(arg, ipaddr_network_order, actual_port);

    //The sequence number we are waiting on after having sent it
	// int seq_num_expected = (old_seq_num+1)%MAX_N_CALLBACK;

	// //printf("seq_num is %d, seq_num_expected is %d", seq_num,seq_num_expected);
	// if(is_ack_packet) {
 //    	//If it is an ack packet and for the sequence number we are waiting for then we update the context
	// 	if(seq_num == seq_num_expected){
	// 		if(gbn_context->waiting) {
	// 			printf("Got acknowledgement.\n");

	// 		}
	// 		gbn_context->seq_start = (gbn_context->seq_start + 1) % MAX_N_CALLBACK;
	// 		gbn_context->num_pcb_stored--;
	// 		if(gbn_context->num_pcb_stored == 0) {
 //    			//stop timer
	// 		}
	// 	}
	// }
}


err_t rdp_gobackn_send (struct udp_pcb *pcb, struct pbuf *p){
	//For now if the current port being used does not have a valid rdp port format (bits captured by mask should be 0)
	//then hardcode the local port to be 5012

	int not_valid_rdp_port = pcb->local_port & RDP_VALID_RDP_PORT_MASK;
	if(not_valid_rdp_port){
		pcb->local_port = 5012;
	}

	//Make sure to only proceed if we're not waiting for the queue to empty
	if(snw_context->waiting) {
		//If not then we refuse data
		printf("Still waiting for queue of messages to free up. Try again later.");
		return ERR_OK;
	}

	//Get next sequence number from the context:
	int cur_seq_num = gbn_context->next_seq_num;
	//printf("current sequence number should be 0, and is %d\n", cur_seq_num);

	//Update it
	gbn_context->next_seq_num = (cur_seq_num + 1) % MAX_N_CALLBACK;
	//printf("updated sequence number in context should be 1, and is %d\n", gbn_context->next_seq_num);
	//printf("do we need to wait? 1 yes, 0 no: %d\n", gbn_context->waiting);
	//If that next_seq_num is back at 0, we must wait since the arrays are full
	if (gbn_context->next_seq_num == 0) {
		gbn_context->waiting = 1;
	}

	//Saving the port:
	uint16_t actual_port = pcb -> local_port;
	//printf("the actual port is: %d\n", actual_port);

	//hiding the seq num in the port:
	pcb -> local_port = pcb -> local_port + 10*cur_seq_num;
	//printf("changed port number, should be 5012+0=5012: %d", pcb -> local_port);

	// //Store pcb into the context:
	gbn_context->pcb[cur_seq_num] = *pcb;
	 
	//printf("checking port indeed 5012: %d\n",(gbn_context->pcb[cur_seq_num]).local_port);
	 

	// //Increase the field carrying the number of pcb's in the pcb array
	gbn_context->num_pcb_stored = gbn_context -> num_pcb_stored + 1;

	// //Copy current payload into the context's payload array
	gbn_context->payload[cur_seq_num] = p->payload;

	//send to udp
	err_t err = udp_send(pcb,p);

	//start the timer
	rdp_timer_start(gbn_timer_context);

	//reseting the port:
	pcb->local_port = actual_port;

	return err;
}

void rdp_gobackn_resend_packet (void *arg){
	struct gobackn_context *context = (struct gobackn_context *) arg;

	// //Retrive the pcb representing the connection from the context
	// // struct udp_pcb *pcbArray[] = context->pcb;
	// struct udp_pcb *pcb_array[MAX_N_CALLBACK];
	// memcpy(pcb_array, context->pcb, MAX_N_CALLBACK);
	// //Allocate a new pbuffer in which to resend the message

	// for(uint16_t i = context->seq_start; i != context->next_seq_num; i = (i+1)%MAX_N_CALLBACK) {
	// 	struct udp_pcb *pcb = pcb_array[i];

	// 	struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, strlen(context->payload[i]), PBUF_RAM);
 //    	//copy the payload into a new buffer
	// 	char payload[DEFAULT_MTU];
	// 	strcpy(payload, context->payload[i]);
	// 	//Set the pbuf's payload
	// 	p->payload = payload;


 //    	//Keep local port to be able to sanitize pcb after all of this
	// 	uint16_t actual_port = pcb->local_port;
	// 	//Add the sequence number that was last send to the port
	// 	pcb->local_port = add_seq_num_to_port(abs(gbn_context->next_seq_num -1)%2, actual_port);

	// 	//Resend the packet
	// 	udp_send(pcb, p);

	// 	//Reset the pcb's local port to the actual port without the sequence number.
	// 	pcb->local_port = actual_port;
	// }

}

void rdp_gobackn_shutdown (){
	if(gbn_context != NULL) {
		if(gbn_context->payload != NULL) {
			free(gbn_context->payload);
		}

		free(gbn_context);
		gbn_context = NULL;
	}
	

	// rdp_timer_shutdown(gbn_timer_context);

}
