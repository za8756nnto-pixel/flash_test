/*****************************************
 * @file uart_print.h
 *
 * UART プリンター関数群のヘッダファイル
 *
 * 作成者: user
 *****************************************/
#ifndef UART_PRINT_H
#define UART_PRINT_H

//----------------------------------
// インクルード
//----------------------------------
#include "driverlib.h"
#include "device.h"

//-----------------------------------
// プロトタイプ宣言
//-----------------------------------
void UART_init(void);                   //UARTの初期化
void UART_printStr(const char *str);    //UARTに文字列を送信
void UART_printHex(uint32_t val);       //ハードウェアでの16ビット数値をUARTで送信

#endif
