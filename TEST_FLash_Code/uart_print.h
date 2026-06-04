/*
 * uart_print.h
 *
 *  Created on: 2026/06/04
 *      Author: user
 */

#ifndef UART_PRINT_H
#define UART_PRINT_H

#include "driverlib.h"
#include "device.h"

extern void UART_init(void);
extern void UART_printStr(const char *str);
extern void UART_printHex(uint32_t val);

#endif
