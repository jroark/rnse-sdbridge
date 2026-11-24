/* The phone over the network: with a Wi-Fi network configured
 * (menuconfig, "Android Auto bridge"), a task joins it and, whenever
 * asked, tries a TCP connection to the phone's head unit server
 * (Android Auto's developer settings; port 5277), the phone's address
 * the one configured or the network's gateway (the phone's own
 * hotspot). */
#ifndef NET_H
#define NET_H
#include <stdint.h>

/* Wi-Fi started, when a network is configured: 1, or 0 with none. */
int net_start(void);
/* Connections are wanted (no phone on USB) or not. */
void net_want(int want);
/* A connection made: its socket (the caller's from then on), or -1. */
int net_take(void);

/* How many times a device has left the network (ours, as the access
 * point): a connection to it does not say so by itself. */
unsigned net_departures(void);

#endif
