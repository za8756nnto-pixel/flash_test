/*
 * uart_print.c
 *
 *  Created on: 2026/06/04
 *      Author: user
 */
#include "uart_print.h"

void UART_init(void)
{
    // GPIO設定（GPIO_setMasterCoreは不要：シングルコアデバイス）
    GPIO_setPinConfig(DEVICE_GPIO_CFG_SCITXDA);
    GPIO_setDirectionMode(DEVICE_GPIO_PIN_SCITXDA, GPIO_DIR_MODE_OUT);
    GPIO_setPadConfig(DEVICE_GPIO_PIN_SCITXDA, GPIO_PIN_TYPE_STD);
    GPIO_setQualificationMode(DEVICE_GPIO_PIN_SCITXDA, GPIO_QUAL_ASYNC);

    GPIO_setPinConfig(DEVICE_GPIO_CFG_SCIRXDA);
    GPIO_setDirectionMode(DEVICE_GPIO_PIN_SCIRXDA, GPIO_DIR_MODE_IN);
    GPIO_setPadConfig(DEVICE_GPIO_PIN_SCIRXDA, GPIO_PIN_TYPE_STD);
    GPIO_setQualificationMode(DEVICE_GPIO_PIN_SCIRXDA, GPIO_QUAL_ASYNC);

    // SCI初期化
    SCI_performSoftwareReset(SCIA_BASE);

    SCI_setConfig(SCIA_BASE,
                  DEVICE_LSPCLK_FREQ,
                  115200,
                  (SCI_CONFIG_WLEN_8 |
                   SCI_CONFIG_STOP_ONE |
                   SCI_CONFIG_PAR_NONE));

    SCI_resetChannels(SCIA_BASE);
    SCI_clearInterruptStatus(SCIA_BASE, SCI_INT_TXFF | SCI_INT_RXFF);
    SCI_enableFIFO(SCIA_BASE);
    SCI_enableModule(SCIA_BASE);
    SCI_performSoftwareReset(SCIA_BASE);
}

void UART_printStr(const char *str)
{
    while(*str != '\0')
    {
        SCI_writeCharBlockingFIFO(SCIA_BASE, (uint16_t)*str);
        str++;
    }
}

void UART_printHex(uint32_t val)
{
    char buf[11];
    const char hex[] = "0123456789ABCDEF";
    int i;

    buf[0]  = '0';
    buf[1]  = 'x';
    buf[10] = '\0';

    for(i = 9; i >= 2; i--)
    {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    UART_printStr(buf);
}

