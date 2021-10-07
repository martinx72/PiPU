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
#include <libusb-1.0/libusb.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include "nesstuff.h"

static libusb_context *ctx = NULL;
struct libusb_device_handle *devh = NULL;

struct libusb_transfer *transfer_out[2] = {NULL, NULL};

int transferIds[2] = {0, 1};
extern PPUFrame outbuf[2];
extern int readBuf;

void cb_out(struct libusb_transfer *transfer);

void trans_right(int num)
{
    libusb_free_transfer(transfer_out[num]);

    transfer_out[num] = libusb_alloc_transfer(0);

    // TODO - need to guarantee that any frame with a palette change doesn't actually send its palette change until the next frame is ready

    libusb_fill_bulk_transfer(transfer_out[num], devh, 0x02, (unsigned char*)&outbuf[readBuf], 41002, cb_out, (void *)&transferIds[num], 0);

    if (libusb_submit_transfer(transfer_out[num]) < 0)
    {
        perror("Couldn't Submit Transfer ");
        exit(1);
    }
}

void cb_out(struct libusb_transfer *transfer)
{
    trans_right(*((int *)transfer->user_data));
}


// Fake a keypress via uinput
void __inline emit(int fd, int type, int code, int val)
{
    struct input_event ie = {0};

    ie.type = type;
    ie.code = code;
    ie.value = val;

    /* timestamp values below are ignored */
    //ie.time.tv_sec = 0;
    //ie.time.tv_usec = 0;

    write(fd, &ie, sizeof(ie));
}

int fd_uinput;
unsigned char indata[3];

// Thread that squirts data to the PPU (and recieves data back)
 static unsigned char 
 GAMEPAD_KEYS[] = {
    PAD_UP,
    PAD_DOWN,
    PAD_LEFT,
    PAD_RIGHT,
    PAD_START,
    PAD_SELECT,
    PAD_A,
    PAD_B
    };

extern unsigned char uc8Keys[8];    

void *Squirt(void *threadid)
{

    int r = 1; // result
    int actual_length;

    // Set up keyboard emulation
    struct uinput_setup usetup;
    fd_uinput = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if ( fd_uinput < 0 )
    {
        fprintf(stderr, "/dev/uinput open failed!\n");
    }
    else
    {
        ioctl(fd_uinput, UI_SET_EVBIT, EV_KEY);
        for ( int i = 0; i < 8; i++)
            ioctl(fd_uinput, UI_SET_KEYBIT, uc8Keys[i]);

        memset(&usetup, 0, sizeof(usetup));
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = 0x1234; 
        usetup.id.product = 0x5678;
        strcpy(usetup.name, "PiPU Virtual Keyboard");
        ioctl(fd_uinput, UI_DEV_SETUP, &usetup);
        ioctl(fd_uinput, UI_DEV_CREATE);
    }

    // Init libUSB
    r = libusb_init(NULL);
    if (r < 0)
    {
        fprintf(stderr, "Failed to initialise libusb\n");
        exit(1);
    }


    // Open USB device

    devh = libusb_open_device_with_vid_pid(ctx, 0x04b4, 0x00F1);
    if (!devh)
    {
        perror("device not found");
        exit(1);
    }


    // Claim the interface

    r = libusb_claim_interface(devh, 0);
    if (r < 0)
    {
        fprintf(stderr, "usb_claim_interface error %d\n", r);
        exit(1);
    }
    else
        printf("Claimed interface\n");

    
    // Do initial transfer, will auto-reload

    transfer_out[0] = libusb_alloc_transfer(0);
    transfer_out[1] = libusb_alloc_transfer(0);

    trans_right(0);
    trans_right(1);

    // Handle reading from PPU bus, for gamepad input
    unsigned char PrevKey = 0x00;
    unsigned char CurrentKey = 0x00;
    unsigned char ucChkKey;

    while (1)
    {
        r = libusb_bulk_transfer(devh, 0x86, indata, sizeof(indata), &actual_length, 0);
        if (r == 0 && actual_length == sizeof(indata))
        {
            CurrentKey = indata[0];
            if  ( PrevKey != CurrentKey ) 
            {
                if ( fd_uinput != -1 )
                {
                    for ( int i = 0; i < 8; i++ )
                    {
                        ucChkKey = CurrentKey & GAMEPAD_KEYS[i];
                        if ( (PrevKey & GAMEPAD_KEYS[i]) != ucChkKey ) 
                            emit(fd_uinput, EV_KEY, uc8Keys[i], ucChkKey ? 1:0);
                    }

                    emit(fd_uinput, EV_SYN, SYN_REPORT, 0);
                }
                PrevKey = CurrentKey;
            }
        }
        else
        {
            printf("Transfer error %s %d\n", libusb_strerror(r), actual_length);
            printf("Data %d : %d %d %d\n", actual_length, indata[0], indata[1], indata[2]);
        }
        if (libusb_handle_events(NULL) != LIBUSB_SUCCESS)
            break;
    }
    return 0;
}

