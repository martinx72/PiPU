#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <linux/uinput.h>
#include <libusb-1.0/libusb.h>
#include "nesstuff.h"

#define NUM_GFX_THREADS 2

threaddata threads[NUM_GFX_THREADS];
int scanlinesperthread = 240 / NUM_GFX_THREADS;

PPUFrame outbuf[2];

struct region *rptr;

void *Squirt(void *threadid);
void FitFrame(char *bmp, PPUFrame *theFrame, int startline, int endline);
void GFXSetup();

pthread_mutex_t outmutex, bufferswitchmutex;
pthread_cond_t outfullcond;
volatile int outbuffull = 0;
unsigned char bDisplayFPS = 0;

// The out buffer that is currently being displayed, for double buffering
int readBuf = 0;

// The out buffer to write to, for double buffering
int writeBuf = 1;

extern unsigned char indata[3];

// Graphics Thread - converts one section of the screen to NES format
void *gfxthread(void *threadid)
{
	struct timeval tv;
	long lastTime = 0;

	int frameCount = 0;

	threaddata *td = (threaddata *)threadid;

	sleep(1);

	int j;

	int startline, endline;
	int tempfull = 0;
	startline = (td->threadno * scanlinesperthread);
	if (startline < 0)
		startline = 0;

	endline = (td->threadno * scanlinesperthread) + (scanlinesperthread);

	if (endline > 240)
		endline = 240;

	printf("thread no %d - lines %d-%d\n", td->threadno, startline, endline);
	//rptr->full = 0;

	// wait for the producer to wake up and initialise mutexes etc
	while (!rptr->full)
	{
		sleep(1);
	}

	while (1)
	{

		// wait for done flag to be reset by thread 0
		if (td->threadno != 0)
		{
			pthread_mutex_lock(&outmutex);

			while (td->outbuffull)
			{
				pthread_cond_wait(&outfullcond, &outmutex);
			}
			pthread_mutex_unlock(&outmutex);
		}

		// wait for input buffer to fill
		pthread_mutex_lock(&rptr->fulllock);
		while (!rptr->full)
		{
			pthread_cond_wait(&rptr->fullsignal, &rptr->fulllock);
		}
		pthread_mutex_unlock(&rptr->fulllock);

		// Display FPS
		if ( bDisplayFPS )
		{
			if (td->threadno == 0)
			{
				frameCount++;
				gettimeofday(&tv, NULL);
				if (tv.tv_sec != lastTime)
				{
					lastTime = tv.tv_sec;
					printf("FPS : %d   Ctrl : %d\n", frameCount, indata[0]);
					frameCount = 0;
				}
			}
		}

		// Convert frame section to NES format
		FitFrame((char*)&rptr->buf, (PPUFrame *)&outbuf[writeBuf], startline, endline);

		// if this is thread zero, wait for other threads to finish then switch the double buffer
		if (td->threadno == 0)
		{
			// Wait for other threads to finish
			pthread_mutex_lock(&outmutex);
			outbuffull = 0;
			while (!outbuffull)
			{
				tempfull = 1;
				for (j = 1; j < NUM_GFX_THREADS; j++)
				{
					if (!threads[j].outbuffull)
					{
						tempfull = 0;
					}
				}
				outbuffull = tempfull;
				if (!outbuffull)
					pthread_cond_wait(&outfullcond, &outmutex);
			}
			pthread_mutex_unlock(&outmutex);


			// mark master input buffer empty
			pthread_mutex_lock(&rptr->fulllock);
			rptr->full = 0;
			pthread_mutex_unlock(&rptr->fulllock);


			// Switch output double buffer
			pthread_mutex_lock(&bufferswitchmutex);
			if (writeBuf)
			{
				writeBuf = 0;
				readBuf = 1;
			}
			else
			{
				writeBuf = 1;
				readBuf = 0;
			}
			pthread_mutex_unlock(&bufferswitchmutex);

			// Mark all output buffers empty
			pthread_mutex_lock(&outmutex);
			for (j = 0; j < NUM_GFX_THREADS; j++)
			{
				threads[j].outbuffull = 0;
			}
			pthread_cond_broadcast(&outfullcond);
			pthread_mutex_unlock(&outmutex);
		}
		else
		{
			// Not thread zero, mark this thread's output buffer full

			pthread_mutex_lock(&outmutex);
			td->outbuffull = 1;
			pthread_cond_broadcast(&outfullcond);
			pthread_mutex_unlock(&outmutex);
		}
	}

	return 0;
}

unsigned char uc8Keys[8] = {
  KEY_UP,          // D-PAD UP
  KEY_DOWN,        // D-PAD Down 
  KEY_LEFT,        // D-PAD Left
  KEY_RIGHT,       // D-PAD Right     
  KEY_ESC,         // Start
  KEY_SPACE,       // Select 
  KEY_LEFTCTRL,    // Btn A
  KEY_LEFTALT      // Btn B
  };

int main(int argc, char **argv)
{

	int fd_sdlrawout;
	pthread_condattr_t cattr;
	pthread_mutexattr_t mattr;
	int rc;
	pthread_t squirthandle;

	// Generate color lookup tables, etc
	GFXSetup();

	if ( (fd_sdlrawout = shm_open("/sdlrawout", O_CREAT | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO)) == -1 )
	{
		perror("shm_open '/sdlrawout' failed");
		exit(1);
	}

	if ( ftruncate(fd_sdlrawout, sizeof(struct region)) == -1 )
	{
		perror("ftruncate '/sdlrawout' failed");
		close(fd_sdlrawout);
		exit(1);
	}

	/* Map shared memory object */
	rptr = mmap(NULL, sizeof(struct region), PROT_READ | PROT_WRITE, MAP_SHARED, fd_sdlrawout, 0);
	if (rptr == MAP_FAILED)
	{
		perror("mmap '/sdlrawout' failed");
		close(fd_sdlrawout);
		exit(1);
	}

	// enable FPS counting via env setting
  const char *envr = getenv("PPUSQUIRT_SHOW_FPS");
	if ((envr) && (strcmp(envr, "1") == 0))
		bDisplayFPS = 1;

  // reading the preset keyboard file if available
  FILE *fpKeyMappingSetting = fopen(argv[1], "rb");
	if ( fpKeyMappingSetting != NULL )
	{
  	fseek(fpKeyMappingSetting, 0L, SEEK_END);
		int nFileSize = ftell(fpKeyMappingSetting);
		fseek(fpKeyMappingSetting, 0L, SEEK_SET);
		if ( nFileSize == 8 )
		{
			printf("Original Key Mapping: ");
			for ( int i = 0; i < 8; i++ )
				printf("0x%02X ", uc8Keys[i]);
			printf("\n");
			fread(uc8Keys, 1, 8, fpKeyMappingSetting);
			printf("Updated Key Mapping:  ");
			for ( int i = 0; i < 8; i++ )
				printf("0x%02X ", uc8Keys[i]);
			printf("\n\n");
		}
		fclose(fpKeyMappingSetting);
	}

	pthread_condattr_init(&cattr);
	pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);

	pthread_mutex_init(&rptr->fulllock, &mattr);
	pthread_cond_init(&rptr->fullsignal, &cattr);

	pthread_mutex_init(&outmutex, NULL);
	pthread_mutex_init(&bufferswitchmutex, NULL);
	pthread_cond_init(&outfullcond, NULL);

	FILE *fp_testframe = fopen("waiting_screen.bgra", "rb");
	fread(&rptr->buf, 307200, 1, fp_testframe);
	fclose(fp_testframe);

	rptr->full = 1;
	
	memset(&outbuf[0], 0x00, sizeof(PPUFrame));
	memset(&outbuf[1], 0x00, sizeof(PPUFrame));

	for (int i = 0; i < NUM_GFX_THREADS; i++)
	{
		threads[i].threadno = i;
		threads[i].outbuffull = 0;
		pthread_mutex_init(&threads[i].donelock, NULL);
		pthread_cond_init(&threads[i].donesignal, NULL);
		rc = pthread_create(&threads[i].handle, NULL, gfxthread, (void *)&threads[i]);

		if (rc)
		{
			printf("ERROR: return code from pthread_create() is %d\n", rc);
			exit(-1);
		}
	}

	rc = pthread_create(&squirthandle, NULL, Squirt, NULL);
	if (rc)
	{
		printf("ERROR: return code from pthread_create() is %d\n", rc);
		exit(-1);
	}

	pthread_join(squirthandle, NULL);
	for (int i = 0; i < NUM_GFX_THREADS; i++)
		pthread_join(threads[i].handle, NULL);
		
	return 1;
}