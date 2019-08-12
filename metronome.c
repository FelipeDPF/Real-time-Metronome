#include <stdio.h>
#include <time.h>
#include <sys/netmgr.h>
#include <sys/neutrino.h>
#include <stdlib.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <math.h>


#define MY_PULSE_CODE _PULSE_CODE_MINAVAIL
#define MY_PAUSE_CODE (MY_PULSE_CODE + 1)
#define MY_QUIT_CODE (MY_PAUSE_CODE + 1)
#define ATTACH_POINT "metronome"

// Declare the following variables that are global to myDevice:
char data[255];
int server_coid;
int beatPerMin =0;
int timeSigTop =0;
int timeSigBot =0;

name_attach_t * attach;

typedef union {
	struct _pulse pulse;
	char msg[255];
} my_message_t;

struct DataTableRow
{
	int timeSigTop;
	int timeSigBot;
	int numofIntervals;
	char Pattern[32];
};

struct DataTableRow t[] = {
		{2, 4, 4, "|1&2&"},
		{3, 4, 6, "|1&2&3&"},
		{4, 4, 8, "|1&2&3&4&"},
		{5, 4, 10, "|1&2&3&4-5-"},
		{3, 8, 6, "|1-2-3-"},
		{6, 8, 6, "|1&a2&a"},
		{9, 8, 9, "|1&a2&a3&a"},
		{12, 8, 12, "|1&a2&a3&a4&a"}
};

typedef struct input {
	int beatPerMin;
	int timeSigTop;
	int timeSigBot;
}input ;

void *metronomeThread(void *arg){
	struct sigevent event;
	struct itimerspec itime;
	timer_t timer_id;
	int rcvid;
	my_message_t msg;

	input * inputs = (input*) arg;

	event.sigev_notify = SIGEV_PULSE;
	event.sigev_coid = ConnectAttach(ND_LOCAL_NODE, 0, attach->chid, _NTO_SIDE_CHANNEL, 0);
	event.sigev_priority = SchedGet(0,0,NULL);
	event.sigev_code = MY_PULSE_CODE;

	timer_create(CLOCK_REALTIME, &event, &timer_id);

	// DO THE equation
	beatPerMin = inputs->beatPerMin;
	timeSigTop = inputs->timeSigTop;
	timeSigBot = inputs->timeSigBot;

	// The metronome would output 120 beats per minute (  60 sec / 120 beats = 0.5 sec / beat).
	double beat =(double) 60 / beatPerMin;
	double perMeasure = beat * 2;
	double perInterval =  (double) perMeasure  / timeSigBot;
	double fractional = perInterval - (int) perInterval;

	//printf( "beat [%.2f]\n",beat);
	//printf( "perMeasure [%.2f]\n",perMeasure);
	//printf( "perInterval [%.2f]\n",perInterval);

	itime.it_value.tv_sec = 1;
	itime.it_value.tv_nsec = 500000000;

	itime.it_interval.tv_sec = perInterval;
	itime.it_interval.tv_nsec = (fractional * 1e+9);

	timer_settime(timer_id, 0, &itime, NULL);

	int index = -1;
	for(int i = 0; i < 8; ++i)
		if (t[i].timeSigTop == inputs->timeSigTop)
			if (t[i].timeSigBot == inputs->timeSigBot)
				index = i;

	if(index == -1){
		printf("Incorrect pattern");
		exit(EXIT_FAILURE);
	}

	for (;;){
		//assign address of str to ptr
		char *ptr;
		ptr=t[index].Pattern;

		while(1){
			// check for a pulse from the user (pause, info or quit)
			rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);
			if (rcvid == 0){
				if (msg.pulse.code == MY_PULSE_CODE)
					printf("%c", *ptr++);

				else if (msg.pulse.code == MY_PAUSE_CODE){
					itime.it_value.tv_sec = msg.pulse.value.sival_int;
					timer_settime(timer_id, 0, &itime, NULL);
				}
				else if (msg.pulse.code == MY_QUIT_CODE){
					printf("\nQuiting\n");

					TimerDestroy(timer_id);
					exit(EXIT_SUCCESS);
				}
				if(*ptr == '\0'){
					printf("\n");
					break;
				}
			}
			fflush( stdout );
		} /* else other messages ... */
	}
	return EXIT_SUCCESS;
}

int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
	int nb;

	nb = strlen(data);

	if (data == NULL)
		return 0;

	//test to see if we have already sent the whole message.
	if (ocb->offset == nb)
		return 0;

	//We will return which ever is smaller the size of our data or the size of the buffer
	nb = min(nb, msg->i.nbytes);

	//Set the number of bytes we will return
	_IO_SET_READ_NBYTES(ctp, nb);

	//Copy data into reply buffer.
	SETIOV(ctp->iov, data, nb);

	//update offset into our data used to determine start position for next read.
	ocb->offset += nb;

	//If we are going to send any bytes update the access time for this resource.
	if (nb > 0)
		ocb->attr->flags |= IOFUNC_ATTR_ATIME;

	return (_RESMGR_NPARTS(1));
}

int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
	int nb = 0;

	if (msg->i.nbytes == ctp->info.msglen - (ctp->offset + sizeof(*msg))){
		/* have all the data */
		char *buf;
		char *alert_msg;
		int i, small_integer;
		buf = (char *)(msg + 1);

		if (strstr(buf, "pause") != NULL){
			for (i = 0; i < 2; i++)
				alert_msg = strsep(&buf, " ");

			small_integer = atoi(alert_msg);
			if (small_integer >= 1 && small_integer <= 9)
				MsgSendPulse(server_coid, SchedGet(0, 0, NULL), MY_PAUSE_CODE, small_integer);
			else
				printf("\nInteger is not between 1 and 9.\n");
		}
		else if (strstr(buf, "info") != NULL){
			sprintf(buf, "metronome [%d beats/min, time signature %d/%d] \n", beatPerMin, timeSigTop, timeSigBot);
			strcpy(data, buf);
		}
		else if (strstr(buf, "quit") != NULL)
			MsgSendPulse(server_coid, SchedGet(0, 0, NULL), MY_QUIT_CODE, 0);
		else
			printf("\nInvalid Command: %s\n", strsep(&buf, "\n"));

		nb = msg->i.nbytes;
	}
	_IO_SET_WRITE_NBYTES(ctp, nb);

	if (msg->i.nbytes > 0)
		ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;

	return (_RESMGR_NPARTS(0));
}

int io_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra)
{
	if ((server_coid = name_open(ATTACH_POINT, 0)) == -1)
	{
		perror("name_open failed.");
		return EXIT_FAILURE;
	}
	return (iofunc_open_default(ctp, msg, handle, extra));
}

int main(int argc, char *argv[])
{
	dispatch_t *dpp;
	resmgr_io_funcs_t io_funcs;
	resmgr_connect_funcs_t connect_funcs;
	iofunc_attr_t ioattr;
	dispatch_context_t *ctp;
	int id;

	attach = name_attach(NULL, ATTACH_POINT, 0);

	if(attach == NULL){
		perror("failed to create the channel.");
		exit(EXIT_FAILURE);
	}

	if (argc != 4){
		perror("Not the correct arguments.");
		exit(EXIT_FAILURE);
	}

	struct input inputs = {
			atoi(argv[1]),
			atoi(argv[2]),
			atoi(argv[3]),
	};

	dpp = dispatch_create();
	iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);

	connect_funcs.open = io_open;
	io_funcs.read = io_read;
	io_funcs.write = io_write;

	iofunc_attr_init(&ioattr, S_IFCHR | 0666, NULL, NULL);
	id = resmgr_attach(dpp, NULL, "/dev/local/metronome", _FTYPE_ANY, NULL, &connect_funcs, &io_funcs, &ioattr);

	ctp = dispatch_context_alloc(dpp);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_create(NULL, &attr, &metronomeThread, &inputs);

	while (1){
		ctp = dispatch_block(ctp);
		dispatch_handler(ctp);
	}

	pthread_attr_destroy(&attr);
	name_detach(attach, 0);

	return EXIT_SUCCESS;
}
