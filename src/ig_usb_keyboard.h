#include <gbdk/platform.h>
#include <stdint.h>
#include <stdbool.h>

#define KEY_NONE        0x00u
#define KEY_NO_CONNECT  0xFFu

#define KEY_AMP    92u
#define KEY_POUND  93u

#define KEY_RIGHT  172u
#define KEY_DOWN   174u
#define KEY_LEFT   173u
#define KEY_UP     175u

#define KEY_ENTER  133u
#define KEY_ESC    134u
#define KEY_BACKSP 135u
#define KEY_SPACE  137u
#define KEY_DEL    169u

#define KEY_PGUP   168u
#define KEY_PGDN   171u

#define KEY_1  123u
#define KEY_2  124u
#define KEY_3  125u
#define KEY_4  126u
#define KEY_5  127u
#define KEY_6  128u
#define KEY_7  129u
#define KEY_8  130u
#define KEY_9  131u
#define KEY_0  132u

#define KEY_F1  151u
#define KEY_F2  152u
#define KEY_F3  153u
#define KEY_F4  154u
#define KEY_F5  155u
#define KEY_F6  156u
#define KEY_F7  157u
#define KEY_F8  158u
#define KEY_F9  159u
#define KEY_F10 160u
#define KEY_F11 161u
#define KEY_F12 162u

bool usb_keyboard_has_data(void) BANKED;
uint8_t usb_keyboard_get_key(void) BANKED;
void usb_keyboard_install(void) BANKED;
void usb_keyboard_deinstall(void) BANKED;