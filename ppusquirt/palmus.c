#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

struct palmusdata {
	unsigned char music;
	unsigned char Palettes[4][3];
};
struct palmusdata *pmdata;

int fd_palmusdata;

int main(int argc, char **argv) {

    /* Create shared memory object and set its size */
    int i, j, a ;

    fd_palmusdata = shm_open("/palmusdata", O_CREAT | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO );
    if (fd_palmusdata == -1)
    {
		/* Handle error */
	}


    if (ftruncate(fd_palmusdata, sizeof(struct palmusdata)) == -1)
	{
		/* Handle error */;
	}

    /* Map shared memory object */
    pmdata = mmap(NULL, sizeof(struct palmusdata), PROT_READ | PROT_WRITE, MAP_SHARED, fd_palmusdata, 0);
    if ( pmdata == MAP_FAILED )
        exit(1);

    a = 1;
    pmdata->music = atoi(argv[a++]);

    for (j = 0; j < 4; j++){
        for (i = 0; i < 3; i++){
            pmdata->Palettes[j][i] = strtol(argv[a++], NULL, 16);
        }
    }

    /* Now we can refer to mapped region using fields of rptr;
    for example, rptr->len */

    printf("%d\n", pmdata->music);
	
	close(fd_palmusdata);

}
