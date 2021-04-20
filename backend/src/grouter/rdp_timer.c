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

/*****************************
RDP TIMER 
*****************************/
void *rdp_run_timer(void *arg) {
	struct rdp_timer_context *context= (struct rdp_timer_context *) arg;
	printf("Running timer threada\n");

	while(!context->shutdown){
		struct timeb start;
		ftime(&start);

		while(context->active && !context->shutdown) {
			struct timeb now;
			ftime(&now);

			int milliseconds = (int) (1000.0 * (now.time - start.time)) + (now.millitm - start.millitm);

			if(milliseconds>=context->time) {
				context->callback(context->callback_arg);
				
				ftime(&start);
			}
		}
	}
	// printf("Freeing the timer context\n");
	free(context);
}

struct rdp_timer_context * rdp_timer_init(int time, rdp_timer_callback callback, void *callback_arg) {
	// printf("Initializing timer\n");
	struct rdp_timer_context *context = (struct rdp_timer_context *) malloc(sizeof(struct rdp_timer_context));

	context->time = time;
	context->active = 0;
	context->shutdown = 0;

	context->callback = callback;
	context->callback_arg = callback_arg;

	pthread_t thread_id;

	pthread_create(&thread_id, NULL, rdp_run_timer, context);

	return context;
}

int rdp_timer_start(struct rdp_timer_context *context) {
	// printf("Starting timer\n");
	context->active = 1;
}

int rdp_timer_reset(struct rdp_timer_context *context) {
	if(context->active){
		// printf("Resetting timer\n");
		context->active = 0;
	}
}

int rdp_timer_shutdown(struct rdp_timer_context *context) {
	// printf("Shutting down timer\n");
	context->shutdown = 1;
}