#include "pbuf.h"

#ifndef __RDP_TIMER_H_
#define __RDP_TIMER_H_

/*****************************
RDP TIMER 
*****************************/
typedef void (*rdp_timer_callback)(void *arg);

struct rdp_timer_context {
	int time;						//How long the timer is
	volatile int active;			//Whether it is active or not
	volatile int shutdown;			//Whether the timer should shut down				
	rdp_timer_callback callback;	//callback that will be called
	void *callback_arg;				//argument to be given to the callback
};

/**
 *	To initialize the timer and have the context ready to be started or reset
 *  The rdp timer callback represents the routine to be run once the time expires
 *	The callback_arg is what gets passed to the routine 
 *  NOTE: THIS ALSO STARTS UP THE TIMER THREAD SO ITS IMPORTANT TO CALL rdp_timer_shutdown once we're done
 */
struct rdp_timer_context* 		rdp_timer_init			(int time, rdp_timer_callback, void *callback_arg);

/**
 *	To start the timer. Once time elapses, the callback fires and then the timer restarts by itself.
 *  In order to actually stop the timer for running it must be reset
 */
int 							rdp_timer_start			(struct rdp_timer_context *context);

/**
 *	To reset the timer. This makes sure that the timer is no longer running and is waiting 
 *  for the next call to start.
 */
int 							rdp_timer_reset			(struct rdp_timer_context *context);


/**
 *  To shutdown the timer. This makes sure that there's a cleanup of all resources.
 *  IT TAKES CARE OF FREEING THE CONTEXT MEMORY
 */
int 							rdp_timer_shutdown		(struct rdp_timer_context *context);



#endif // ifndef __RDP_TIMER_H_