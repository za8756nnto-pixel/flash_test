/*****************************************
 * @file uart_print.ｃ
 *
 * UART プリンター関数群
 *
 * 作成者: user
 *****************************************/

//----------------------------------
// インクルード
//----------------------------------
#include "uart_print.h"



/*******************************************************
 * @brief UARTモジュール（SCIA）を初期化する
 *
 * GPIO端子のピン設定・方向・パッド・同期モードを構成した後、
 * SCIモジュールを115200bps / 8bit / ストップ1bit / パリティなし で
 * 初期化し、FIFOを有効化して通信可能状態にする。
 *
 *******************************************************/
void UART_init(void)
{
    // GPIO設定（GPIO_setMasterCoreは不要：シングルコアデバイス）
    GPIO_setPinConfig(DEVICE_GPIO_CFG_SCITXDA);                             //TXピンをSCI機能に割り当て
    GPIO_setDirectionMode(DEVICE_GPIO_PIN_SCITXDA, GPIO_DIR_MODE_OUT);      //TXピンを出力方向に設定
    GPIO_setPadConfig(DEVICE_GPIO_PIN_SCITXDA, GPIO_PIN_TYPE_STD);          //プッシュプル出力（標準）に設定
    GPIO_setQualificationMode(DEVICE_GPIO_PIN_SCITXDA, GPIO_QUAL_ASYNC);    //サンプリング同期なし（非同期）に設定

    GPIO_setPinConfig(DEVICE_GPIO_CFG_SCIRXDA);                             //RXピンをSCI機能に割り当て
    GPIO_setDirectionMode(DEVICE_GPIO_PIN_SCIRXDA, GPIO_DIR_MODE_IN);       //RXピンを入力方向に設定
    GPIO_setPadConfig(DEVICE_GPIO_PIN_SCIRXDA, GPIO_PIN_TYPE_STD);          //標準入力パッド（プルなし）に設定
    GPIO_setQualificationMode(DEVICE_GPIO_PIN_SCIRXDA, GPIO_QUAL_ASYNC);    //サンプリング同期なし（非同期）に設定

    // SCI初期化
    //SCIモジュールをソフトウェアリセットして初期状態に戻す
    SCI_performSoftwareReset(SCIA_BASE);

    SCI_setConfig(SCIA_BASE,
                  DEVICE_LSPCLK_FREQ,       /* ペリフェラルクロック周波数を指定 */
                  115200,                   /* ボーレートを115200bpsに設定 */
                  (SCI_CONFIG_WLEN_8 |      /* データ長：8bit */
                   SCI_CONFIG_STOP_ONE |    /* ストップビット：1bit */
                   SCI_CONFIG_PAR_NONE));   /* パリティ：なし */
    //TXチャネル・RXチャネルをリセットしてFIFOをクリア
    SCI_resetChannels(SCIA_BASE);
    SCI_clearInterruptStatus(SCIA_BASE, SCI_INT_TXFF | SCI_INT_RXFF);   //TX/RX FIFOの割り込みフラグをクリア
    SCI_enableFIFO(SCIA_BASE);                                          //FIFOバッファを有効化（送受信効率を向上）
    SCI_enableModule(SCIA_BASE);                                        //SCIモジュールを有効化して動作開始
    SCI_performSoftwareReset(SCIA_BASE);                                //設定確定後に再リセットし、送受信ステートマシンを初期化
}

/*****************************************************
 * @brief 文字列をUART（SCIA）へ送信する
 *
 * 引数で渡された文字列を1文字ずつ、FIFOブロッキング送信で出力する。
 * ヌル終端文字（'\0'）に達した時点で送信を終了する。
 *
 * @param[in] str 送信するヌル終端文字列へのポインタ
 *****************************************************/
void UART_printStr(const char *str)
{
    //終端に達するまでループ
    while(*str != '\0')
    {
        //1文字をFIFOへ書き込み（FIFOが満杯なら空くまで待機）
        SCI_writeCharBlockingFIFO(SCIA_BASE, (uint16_t)*str);
        //次の文字へポインタを進める
        str++;
    }
}

/******************************************************
 * @brief 32bit整数値を16進数文字列に変換してUARTへ送信する
 *
 * "0x" プレフィックス付きの8桁大文字16進数（例: 0x1A2B3C4D）として出力する。
 * 内部バッファ（11バイト）に文字列を組み立てた後、UART_printStr() で送信する。
 *
 * @param[in] val 送信する32bit符号なし整数値
 *****************************************************/
void UART_printHex(uint32_t val)
{
    char buf[11];                           //"0x" + 8桁の16進数 + ヌル終端 = 11バイト
    const char hex[] = "0123456789ABCDEF";  //16進数変換用の文字テーブル
    int i;

    buf[0]  = '0';      //プレフィックス "0x" の '0' をセット
    buf[1]  = 'x';      //プレフィックス "0x" の 'x' をセット
    buf[10] = '\0';     //文字列末尾にヌル終端をセット

    //下位4bit（ニブル）ずつ取り出し、末尾から順に16進文字を格納する
    for(i = 9; i >= 2; i--)
    {
        buf[i] = hex[val & 0xF];    //最下位ニブルを16進文字に変換してバッファへ格納
        val >>= 4;                  //次のニブルを最下位へシフト
    }
    UART_printStr(buf);             //組み立てた16進数文字列をUARTへ送信
}

