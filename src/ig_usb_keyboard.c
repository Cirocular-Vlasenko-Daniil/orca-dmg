#include <gbdk/platform.h>
#include <stdint.h>
#include <stdbool.h>
#include "ig_usb_keyboard.h"

#pragma bank 1

#define SIO_BUF_SZ          4u
#define SIO_BUF_COUNT_RESET 0u
#define SIO_BUF_START       0u
#define SIO_IDX_HANDLE_WRAP(idx) if (idx == SIO_BUF_SZ) { idx = SIO_BUF_START; }

static void vbl_keyboard_handler(void);
static void sio_keyboard_handler(void);

static uint8_t sio_rx_data[SIO_BUF_SZ];
static uint8_t sio_write_head;
static uint8_t sio_read_tail;
static volatile uint8_t sio_count = SIO_BUF_COUNT_RESET;


static void vbl_keyboard_handler(void) NONBANKED {
    // Wait for any existing transfers in progress to complete
    while (SC_REG & SIOF_XFER_START);

    // Start a new transfer, the SIO interrupt will fire when it's done
    SB_REG = 0x00u;
    SC_REG = SIOF_XFER_START | SIOF_CLOCK_INT;
}


// Triggers on received serial data which should be once per frame
// to keep overhead low. Transfer is initiated in VBlank.
static void sio_keyboard_handler(void) NONBANKED {

    // Save serial link in the buffer if there is room
    // and it is an actual key
    uint8_t rx_key = SB_REG;
    if ((rx_key > KEY_NONE) && (rx_key < KEY_NO_CONNECT)) {
        if (sio_count < SIO_BUF_SZ) {
            sio_count++;
            sio_rx_data[sio_write_head] = SB_REG;
            // Wrap around if end was reached
            sio_write_head++;
            SIO_IDX_HANDLE_WRAP(sio_write_head);
        }
    }
}


// Returns non-zero if keyboard
// data is ready for use
bool usb_keyboard_has_data(void) BANKED {
    return (sio_count != 0);
}


// Returns keyboard data if available
// otherwise KEY_NONE
uint8_t usb_keyboard_get_key(void) BANKED {

    if (sio_count) {
        volatile uint8_t ret_key;
        CRITICAL {
            ret_key = sio_rx_data[sio_read_tail];
            sio_count--;
        }
        // Tail wrap doesn't need to be in critical section
        // since it's only interacted with by main
        sio_read_tail++;
        SIO_IDX_HANDLE_WRAP(sio_read_tail);
        return ret_key;
    }
    else
        return KEY_NONE;
}


void usb_keyboard_install(void) BANKED {

    CRITICAL {
        sio_write_head = SIO_BUF_START;
        sio_read_tail  = SIO_BUF_START;
        sio_count      = SIO_BUF_COUNT_RESET;

        // Remove first to avoid accidentally double-adding the interrupt handlers
        remove_VBL(vbl_keyboard_handler);
        remove_SIO(sio_keyboard_handler);

        add_VBL(vbl_keyboard_handler);
        add_SIO(sio_keyboard_handler);
    }

    // Enable Serial interrupt
    set_interrupts(IE_REG | SIO_IFLAG);
}


void usb_keyboard_deinstall(void) BANKED {

    CRITICAL {
        sio_count = SIO_BUF_COUNT_RESET;

        remove_VBL(vbl_keyboard_handler);
        remove_SIO(sio_keyboard_handler);
    }

    // Enable Serial interrupt
    set_interrupts(IE_REG & ~SIO_IFLAG);
}
