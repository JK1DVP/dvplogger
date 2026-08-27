#include "usb_cat_transport.h"
#include "decl.h"
#include "variables.h"

static usb_cat_backend_t current_backend = USB_CAT_BACKEND_NONE;

static const usb_cat_profile_t profiles[] = {
  { USB_CAT_BACKEND_NONE,        "none",     false, false, false, nullptr },
  { USB_CAT_BACKEND_ACM_GENERIC, "ACM",      false, false, false, nullptr },
  { USB_CAT_BACKEND_ACM_QMX,     "QMX ACM",  true,  true,  true,  "IF;" },
  { USB_CAT_BACKEND_CP2105,      "CP2105",   true,  false, false, nullptr }
};

const usb_cat_profile_t &usb_cat_profile(usb_cat_backend_t backend)
{
  const size_t idx = static_cast<size_t>(backend);
  if (idx >= sizeof(profiles) / sizeof(profiles[0])) return profiles[0];
  return profiles[idx];
}

void usb_cat_set_backend(usb_cat_backend_t backend)
{
  if (current_backend == backend) return;
  current_backend = backend;
  if (verbose & VERBOSE_USB) {
    console->printf("USB CAT backend=%s\n", usb_cat_backend_label());
  }
}

usb_cat_backend_t usb_cat_backend() { return current_backend; }
const char *usb_cat_backend_label() { return usb_cat_profile(current_backend).label; }

UBaseType_t usb_cat_reset_tx_queue()
{
  if (xQueueCATUSBTx == nullptr) return 0;
  const UBaseType_t n = uxQueueMessagesWaiting(xQueueCATUSBTx);
  xQueueReset(xQueueCATUSBTx);
  return n;
}

bool usb_cat_enqueue(const uint8_t *data, size_t len,
                     bool drop_oldest, bool *dropped_oldest)
{
  if (dropped_oldest) *dropped_oldest = false;
  if (xQueueCATUSBTx == nullptr || data == nullptr || len == 0 ||
      len > sizeof(((catmsg_t *)nullptr)->buf)) return false;

  catmsg_t msg = {};
  msg.size = static_cast<int>(len);
  memcpy(msg.buf, data, len);

  BaseType_t ret = xQueueSend(xQueueCATUSBTx, &msg, 0);
  if (ret != pdTRUE && drop_oldest) {
    catmsg_t stale = {};
    if (xQueueReceive(xQueueCATUSBTx, &stale, 0) == pdTRUE) {
      if (dropped_oldest) *dropped_oldest = true;
      ret = xQueueSend(xQueueCATUSBTx, &msg, 0);
    }
  }
  return ret == pdTRUE;
}

bool usb_cat_dequeue(catmsg_t *msg)
{
  return msg != nullptr && xQueueCATUSBTx != nullptr &&
         xQueueReceive(xQueueCATUSBTx, msg, 0) == pdTRUE;
}

bool usb_cat_requeue_front(const catmsg_t *msg)
{
  return msg != nullptr && xQueueCATUSBTx != nullptr &&
         xQueueSendToFront(xQueueCATUSBTx, msg, 0) == pdTRUE;
}

bool usb_cat_deliver_rx(const uint8_t *data, size_t len)
{
  if (xQueueCATUSBRx == nullptr || data == nullptr || len == 0) return false;
  catmsg_t msg = {};
  msg.size = static_cast<int>(min(len, sizeof(msg.buf)));
  memcpy(msg.buf, data, msg.size);
  return xQueueSend(xQueueCATUSBRx, &msg, 0) == pdTRUE;
}

UBaseType_t usb_cat_tx_waiting()
{
  return xQueueCATUSBTx ? uxQueueMessagesWaiting(xQueueCATUSBTx) : 0;
}

UBaseType_t usb_cat_tx_free()
{
  return xQueueCATUSBTx ? uxQueueSpacesAvailable(xQueueCATUSBTx) : 0;
}

void usb_cat_dump(const char *direction, const uint8_t *data, size_t len)
{
  if (!(verbose & VERBOSE_USB) || data == nullptr) return;
  console->printf("%s USB CAT %s len=%u ascii=\"",
                  usb_cat_backend_label(), direction ? direction : "?",
                  static_cast<unsigned int>(len));
  for (size_t i = 0; i < len; ++i) {
    const uint8_t c = data[i];
    console->print((c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '.');
  }
  console->print("\" hex=");
  for (size_t i = 0; i < len; ++i) console->printf("%02X ", data[i]);
  console->println();
}
