#ifndef NET_UTILS_H
#define NET_UTILS_H

#include <stdbool.h>
#include <stdint.h>

void wifi_init(void);
void sync_time(void);
void make_hmac(const char *token, char *output);
bool get_device_mac_string(char *output, int output_size);
bool parse_mac_string(const char *input, uint8_t *mac);
bool send_wake_on_lan(const unsigned char *mac);

#endif
