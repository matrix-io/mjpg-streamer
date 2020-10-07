/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <linux/types.h> /* for videodev2.h */
#include <linux/videodev2.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#include "sharedmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*  C-style functions declaration */

#define INPUT_PLUGIN_NAME "WYZE CAM input plugin"

/* private functions and variables to this plugin */
static pthread_t worker;
static globals *pglobal;
static pthread_mutex_t controls_mutex;
static int plugin_number;

const int  maxsize_ =  400 * 1024;

char buffer_[maxsize_];


/*** plugin interface functions ***/

/******************************************************************************
Description.: parse input parameters
Input Value.: param contains the command line string and a pointer to globals
Return Value: 0 if everything is ok
******************************************************************************/

extern "C" int input_init(input_parameter *param, int plugin_no);
extern "C" int input_stop(int id);
extern "C" int input_run(int id);
extern "C" void help(void);
void *worker_thread(void *arg);
void worker_cleanup(void *arg);

int input_init(input_parameter *param, int plugin_no) {
  int i;
  int width = 640, height = 480;

  if (pthread_mutex_init(&controls_mutex, NULL) != 0) {
    IPRINT("could not initialize mutex variable\n");
    exit(EXIT_FAILURE);
  }

  param->argv[0] = INPUT_PLUGIN_NAME;

  /* show all parameters for DBG purposes */
  for (i = 0; i < param->argc; i++) {
    DBG("argv[%d]=%s\n", i, param->argv[i]);
  }

  reset_getopt();
  while (1) {
    int option_index = 0, c = 0;
    static struct option long_options[] = {
        {"h", no_argument, 0, 0},
        {"help", no_argument, 0, 0},
        {"r", required_argument, 0, 0},
        {"resolution", required_argument, 0, 0},
        {0, 0, 0, 0}};

    c = getopt_long_only(param->argc, param->argv, "", long_options,
                         &option_index);

    /* no more options to parse */
    if (c == -1) break;

    /* unrecognized option */
    if (c == '?') {
      help();
      return 1;
    }

    switch (option_index) {
        /* h, help */
      case 0:
      case 1:
        DBG("case 0,1\n");
        help();
        return 1;
        break;

      /* d, device */
      case 2:
      case 3:
        DBG("case 2,3\n");
        break;

      /* f, fps */
      case 4:
      case 5:
        DBG("case 4,5\n");
        break;

      /* r, resolution */
      case 6:
      case 7:
        DBG("case 6,7\n");
        //parse_resolution_opt(optarg, &width, &height);
        break;

      default:
        DBG("default case\n");
        help();
        return 1;
    }
  }

  pglobal = param->global;


  return 0;
}

/******************************************************************************
Description.: stops the execution of the worker thread
Input Value.: -
Return Value: 0
******************************************************************************/
int input_stop(int id) {
  DBG("will cancel input thread\n");
  pthread_cancel(worker);

  return 0;
}

/******************************************************************************
Description.: starts the worker thread and allocates memory
Input Value.: -
Return Value: 0
******************************************************************************/
int input_run(int id) {
  pglobal->in[id].buf = (unsigned char *)malloc(256 * 1024);
  if (pglobal->in[id].buf == NULL) {
    fprintf(stderr, "could not allocate memory\n");
    exit(EXIT_FAILURE);
  }

  if (pthread_create(&worker, 0, worker_thread, NULL) != 0) {
    free(pglobal->in[id].buf);
    fprintf(stderr, "could not start worker thread\n");
    exit(EXIT_FAILURE);
  }
  pthread_detach(worker);

  return 0;
}

/******************************************************************************
Description.: print help message
Input Value.: -
Return Value: -
******************************************************************************/
void help(void) {
  fprintf(
      stderr,
      " ---------------------------------------------------------------\n"
      " Help for input plugin..: " INPUT_PLUGIN_NAME
      "\n"
      " ---------------------------------------------------------------\n"
      " The following parameters can be passed to this plugin:\n\n"
      " [-r | --resolution]....: can be 960x720, 640x480, 320x240, 160x120\n"
      " ---------------------------------------------------------------\n");
}

/******************************************************************************
Description.: copy a picture from testpictures.h and signal this to all output
              plugins, afterwards switch to the next frame of the animation.
Input Value.: arg is not used
Return Value: NULL
******************************************************************************/
void *worker_thread(void *arg) {
  struct timeval timestamp;
  /* set cleanup handler to cleanup allocated resources */
  pthread_cleanup_push(worker_cleanup, NULL);

  while (!pglobal->stop) {
    timeval timeout;
    timeout.tv_usec = 200000;
    {
      /* copy JPG picture to global buffer */
      pthread_mutex_lock(&pglobal->in[plugin_number].db);

      SharedMem& mem = SharedMem::instance();
      
      int memlen = mem.getImageBuffer((void*)buffer_,  maxsize_);
          
      if(memlen)
      {
        pglobal->in[plugin_number].size = memlen;
        memcpy(pglobal->in[plugin_number].buf, buffer_, memlen);
        
        gettimeofday(&timestamp, NULL);
        
        pglobal->in[plugin_number].timestamp = timestamp;
        
        printf("frame (size: %d)\n", pglobal->in[plugin_number].size);
        /* signal fresh_frame */
        pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);        
      }

      pthread_mutex_unlock(&pglobal->in[plugin_number].db);
    }
  }

  IPRINT("leaving input thread, calling cleanup function now\n");
  pthread_cleanup_pop(1);

  return NULL;
}

/******************************************************************************
Description.: this functions cleans up allocated resources
Input Value.: arg is unused
Return Value: -
******************************************************************************/
void worker_cleanup(void *arg) {
  static unsigned char first_run = 1;

  if (!first_run) {
    DBG("already cleaned up resources\n");
    return;
  }

  first_run = 0;
  DBG("cleaning up resources allocated by input thread\n");

  if (pglobal->in[plugin_number].buf != NULL)
    free(pglobal->in[plugin_number].buf);
}

#ifdef __cplusplus
}
#endif
