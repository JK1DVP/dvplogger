#ifndef CP932_UTF8_H
#define CP932_UTF8_H
#include <stddef.h>
#include <stdint.h>
bool is_valid_utf8_bytes(const uint8_t *src, size_t len);
size_t cp932_to_utf8(const uint8_t *src, size_t src_len, char *dst, size_t dst_size);
#endif
