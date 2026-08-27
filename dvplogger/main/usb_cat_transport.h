/*
 * Common USB CAT transport helpers.
 *
 * Keeps queue ownership and device-specific policy out of the rig protocol
 * parsers.  QMX uses the ACM_QMX profile today; Yaesu/CP2105 can use the
 * CP2105 profile once the CAT interface is identified from descriptors.
 */
#ifndef FILE_USB_CAT_TRANSPORT_H
#define FILE_USB_CAT_TRANSPORT_H

#include "Arduino.h"
#include "cat.h"

enum usb_cat_backend_t : uint8_t {
  USB_CAT_BACKEND_NONE = 0,
  USB_CAT_BACKEND_ACM_GENERIC,
  USB_CAT_BACKEND_ACM_QMX,
  USB_CAT_BACKEND_CP2105
};

struct usb_cat_profile_t {
  usb_cat_backend_t backend;
  const char *label;
  bool ascii_cat;
  bool drop_oldest_poll;
  bool skip_cdc_control;
  const char *startup_query;
};

const usb_cat_profile_t &usb_cat_profile(usb_cat_backend_t backend);
void usb_cat_set_backend(usb_cat_backend_t backend);
usb_cat_backend_t usb_cat_backend();
const char *usb_cat_backend_label();

UBaseType_t usb_cat_reset_tx_queue();
bool usb_cat_enqueue(const uint8_t *data, size_t len,
                     bool drop_oldest, bool *dropped_oldest = nullptr);
bool usb_cat_dequeue(struct catmsg_t *msg);
bool usb_cat_requeue_front(const struct catmsg_t *msg);
bool usb_cat_deliver_rx(const uint8_t *data, size_t len);
UBaseType_t usb_cat_tx_waiting();
UBaseType_t usb_cat_tx_free();
void usb_cat_dump(const char *direction, const uint8_t *data, size_t len);

#endif
