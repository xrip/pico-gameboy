/*
 * USB HID keyboard (TinyUSB host) for boards whose keyboard is on USB,
 * such as the Olimex PICO-PC (KBDUSB). Boot-protocol keyboard reports are
 * fed to the same handler as the PS/2 keyboard (process_kbd_report).
 *
 * Based on the TinyUSB host HID example, (c) 2021 Ha Thach (tinyusb.org),
 * MIT License.
 */
#ifdef KBDUSB

#include "tusb.h"

void process_kbd_report(hid_keyboard_report_t const *report,
                        hid_keyboard_report_t const *prev_report);

struct hid_kbd_info_t {
    uint8_t dev_addr;
    uint8_t instance;
    hid_keyboard_report_t prev;
};

static hid_kbd_info_t kbd_info[CFG_TUH_HID] = {};

static hid_kbd_info_t *find_info(uint8_t dev_addr, uint8_t instance, bool allocate) {
    for (hid_kbd_info_t &i : kbd_info)
        if (i.dev_addr == dev_addr && i.instance == instance) return &i;
    if (!allocate) return nullptr;
    for (hid_kbd_info_t &i : kbd_info)
        if (i.dev_addr == 0) {
            i = {};
            i.dev_addr = dev_addr;
            i.instance = instance;
            return &i;
        }
    return nullptr;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report; (void)desc_len;
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD)
        find_info(dev_addr, instance, true);
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    hid_kbd_info_t *i = find_info(dev_addr, instance, false);
    if (i) {
        /* release every key that was held on the unplugged keyboard */
        hid_keyboard_report_t none = {};
        process_kbd_report(&none, &i->prev);
        *i = {};
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    if (report && len >= sizeof(hid_keyboard_report_t) &&
        tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        hid_kbd_info_t *i = find_info(dev_addr, instance, true);
        auto const *kbd = reinterpret_cast<hid_keyboard_report_t const *>(report);
        process_kbd_report(kbd, i ? &i->prev : kbd);
        if (i) i->prev = *kbd;
    }
    tuh_hid_receive_report(dev_addr, instance);
}

#endif /* KBDUSB */
