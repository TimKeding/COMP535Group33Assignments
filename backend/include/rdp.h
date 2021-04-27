#include "pbuf.h"

#ifndef __RDP_H_
#define __RDP_H_

#define RDP_ACK_MASK					0x0001		//The mask for recovering whether this is an ack or not from the port number
#define RDP_SEQ_NUM_MASK				0x0002		//The mask for the seq number in the port number
#define RDP_VALID_RDP_PORT_MASK			0x0003		//The mask to make sure that a port is a proper rdp port

/*****************************
GENERAL RDP INTERFACE
*****************************/

/**
*	Initializes the main rdp values that will be used.
*/
void			 rdp_init						(int window_size, long timeout);

/**
 * Generates a port number that encodes the given sequence numbert
 */
uint16_t		 add_seq_num_to_port			(uint16_t seq_num, uint16_t port);

/**
 * Parses out the port number from the RDP port number
 */
uint16_t		 get_actual_port_from_rdp_port	(uint16_t rdp_port);

/**
 * Returns a port number that now includes the ack flag
 */
uint16_t		 add_ack_to_port				(uint16_t port);

/**
 * Parses out the ack flag from the RDP port number
 */
uint16_t		 get_ack_flag_from_rdp_port		(uint16_t rdp_port);

/**
 * Parses out the sequence number from the RDP port number
 */
uint16_t		 get_seq_num_from_rdp_port		(uint16_t rdp_port);	 

/**
 * To clean up the resources used by the rdp once the program is done. Takes care of freeing up any memory it needs to
 */
void			 rdp_shutdown 					();
/**
 * For debugging
 */
void 			 print_pcb						(struct udp_pcb *pcb);


/*****************************
RDP STOP AND WAIT INTERFACE
*****************************/
struct stopnwait_context {
	int waiting;						//0 if not waiting, 1 if waiting for a ack
	uint16_t next_seq_num;				//The next sequence number to use for sending
	struct udp_pcb *pcb;				//The pcb sent
	char *payload;						//The payload that was sent
	uint16_t seq_num_expected_to_recv;	//The seq num the receiver is expected to get
};

/**
 * To initialize the stop and wait context
 */
struct stopnwait_context*		rdp_stopnwait_init				();

/**
 * The receive callback that runs the stop and wait routine
 */
void			 				rdp_stopnwait_recv_callback		(void *arg, struct udp_pcb *pcb, struct pbuf *p, uchar *addr, uint16_t port);
/**
 * To send using the stop and wait procedure.
 */
err_t            				rdp_stopnwait_send       		(struct udp_pcb *pcb, struct pbuf *p);

/**
 * Resends the last packet based off of the stopnwait_context it is given as an argument
 */
void 			 				rdp_stopnwait_resend_packet		(void *arg);

/**
 * Cleanup when shutting down the service
 */
void							rdp_stopnwait_shutdown			();



/*****************************
RDP GO BACK N INTERFACE
*****************************/

struct gobackn_context {
	int send_base;	/*new*/					//earliest packet sent for which we wait for ack
	int next_seq_num;				        //next packet ready to be sent
	int waiting;  /*may not be necessary*/	//0 if not waiting, 1 if waiting for a ack
	struct udp_pcb *pcb;				    //The pcb sent
	char *payload;						    //The payload that was sent
	uint16_t seq_num_expected_to_recv;	    //The seq num the receiver is expected to get
};
/**
 * To initialize the go-back-n context
 */
struct gobackn_context*			rdp_gobackn_init				();

/**
 * The receive callback that runs the go-back-n routine
 */
void			 				rdp_gobackn_recv_callback		(void *arg, struct udp_pcb *pcb, struct pbuf *p, uchar *addr, uint16_t port, int seq_num);
/**
 * To send using the go-back-n procedure.
 */
err_t            				rdp_gobackn_send     	  		(struct udp_pcb *pcb, struct pbuf *p);

/**
 * Resends the last packet based off of the go-back-n if timeout. It is given as an argument
 */
void 			 				rdp_gobackn_resend_packet		(void *arg);

/**
 * Cleanup when shutting down the service
 */
void							rdp_gobackn_shutdown			();

#endif // ifndef __RD_H_
