/**
 * @file    i2c_slave_protocol_example.c
 * @brief   F2800157 I2Cスレーブ: 複数コマンドに対応したWRITE受信/READ応答の実装例。
 *
 * @details
 *  この例は i2c_slave_preload_example.c で示した「先入れ」方式をベースに、
 *  複数バイトのWRITEコマンド(CMD+LEN+ペイロード)と、複数バイトのREAD応答
 *  (1バイト目は先入れ、2バイト目以降はXRDYで逐次供給)を組み合わせたもの。
 *
 *  対応コマンド:
 *
 *   [WRITE系: CMD, LEN, ペイロード(末尾1byteはCRC)]
 *    0x10  LEN=0x41(65)  データ64byte + CRC1byte  → g_buf10[0..63]=データ, g_buf10[64]=CRC
 *    0x12  LEN=0x02( 2)  データ 1byte + CRC1byte  → g_buf12[0]   =データ, g_buf12[1] =CRC
 *
 *   [READ系: CMDのみWRITEし、NoStop+リピートスタートでREAD]
 *    0x11  65byte応答 = g_buf10[0..64]  (0x10で書き込んだ内容をそのまま返す)
 *    0x13  10byte応答 = g_buf13[0..9]   (起動時に設定したバージョン+CRC、固定値)
 *    0x14   2byte応答 = g_buf14[0..1]   (1byteステータス+CRC、運用中に更新される)
 *
 *  先入れのポイント:
 *   - READ系コマンド(0x11/0x13/0x14)のCMDバイトをRRDYで受信した「その場で」、
 *     応答バッファの先頭1バイトをI2CDXRへ書き込む。これでリピートスタート後の
 *     ADDR+R直後でも、クロックストレッチに頼らず正しいデータが出る。
 *   - 2バイト目以降は、XRDY(前のバイトの送信完了=次のバイトが必要)が来るたびに
 *     バッファから1バイトずつ供給する。バイト間は1バイト分(9 SCLクロック)の
 *     時間的余裕があるので、こちらはストレッチに頼らなくても通常間に合う。
 *
 *  注意:
 *   - レジスタ/ビットフィールド名は f280015x_i2c.h で必ず確認すること
 *     (I2CSTR.bit.AAS / SDIR / RRDY / XRDY / SCD, I2CDRR, I2CDXR, I2CSAR 等)。
 *   - CRCの計算関数 calcCRC8() はダミー実装。実際にマスター側と合わせている
 *     CRCアルゴリズムに置き換えること(多項式・初期値・反転の有無など)。
 *   - 未知のCMDコードや、LEN不一致など異常系の扱いは最小限。
 *     実運用では要件に応じてエラーハンドリングを強化すること。
 */

#include <stdint.h>
#include "F28x_Project.h"   /* I2caRegs, PieCtrlRegs 等の定義を使用する想定 */

/*==========================================================================*/
/* コマンドコード定義                                                        */
/*==========================================================================*/

#define CMD_WRITE_BLOCK     (0x10U)   /* 64byteブロック書き込み           */
#define CMD_READ_BLOCK      (0x11U)   /* 上記ブロックの読み出し(64+CRC)   */
#define CMD_WRITE_SINGLE    (0x12U)   /* 1byte書き込み                    */
#define CMD_READ_VERSION    (0x13U)   /* バージョン読み出し(9+CRC)        */
#define CMD_READ_STATUS     (0x14U)   /* ステータス読み出し(1+CRC)        */

/*==========================================================================*/
/* バッファ定義                                                              */
/*==========================================================================*/

#define BUF10_LEN   (0x41U)   /* 0x10書き込み/0x11応答: データ64 + CRC1 = 65 */
#define BUF12_LEN   (0x02U)   /* 0x12書き込み:          データ1  + CRC1 = 2  */
#define BUF13_LEN   (0x0AU)   /* 0x13応答:              データ9  + CRC1 = 10 */
#define BUF14_LEN   (0x02U)   /* 0x14応答:              データ1  + CRC1 = 2  */

/** @brief CMD 0x10で受信したデータ(0..63)+CRC(64) */
static uint8_t g_buf10[BUF10_LEN];

/** @brief CMD 0x12で受信したデータ(0)+CRC(1) */
static uint8_t g_buf12[BUF12_LEN];

/** @brief CMD 0x13応答用: バージョン(0..8)+CRC(9)。起動時に一度だけ設定する */
static uint8_t g_buf13[BUF13_LEN];

/** @brief CMD 0x14応答用: ステータス(0)+CRC(1)。状態が変わるたびに更新する */
static uint8_t g_buf14[BUF14_LEN];

/*==========================================================================*/
/* CRC (ダミー実装: 実際のアルゴリズムに置き換えること)                        */
/*==========================================================================*/

/**
 * @brief CRC8計算(ダミー実装。マスター側と一致するアルゴリズムに置き換える)
 * @param pData 対象データ
 * @param len   対象データのバイト数
 * @return CRC8値
 */
static uint8_t calcCRC8(const uint8_t *pData, uint16_t len)
{
    uint8_t crc = 0x00U;
    uint16_t i;

    for (i = 0U; i < len; i++)
    {
        crc ^= pData[i];   /* TODO: 実際のCRC8多項式演算に置き換える */
    }

    return crc;
}

/*==========================================================================*/
/* コマンドテーブル                                                          */
/*==========================================================================*/

/** @brief WRITE系コマンド(CMD+LEN+ペイロード)のテーブル要素 */
typedef struct
{
    uint8_t  cmd;        /**< コマンドコード                         */
    uint16_t totalLen;   /**< LENとして受信すべき値(データ+CRCの合計) */
    uint8_t *pBuf;        /**< 格納先(末尾1byteがCRC格納位置)         */
} WriteCmdEntry;

static const WriteCmdEntry g_writeCmdTable[] =
{
    { CMD_WRITE_BLOCK,  BUF10_LEN, g_buf10 },
    { CMD_WRITE_SINGLE, BUF12_LEN, g_buf12 },
};
#define WRITE_CMD_TABLE_NUM  (sizeof(g_writeCmdTable) / sizeof(g_writeCmdTable[0]))

/** @brief READ系コマンド(CMDのみ→READ)のテーブル要素 */
typedef struct
{
    uint8_t        cmd;       /**< コマンドコード           */
    const uint8_t *pBuf;       /**< 応答バッファ先頭         */
    uint16_t       totalLen;  /**< 応答の合計バイト数        */
} ReadCmdEntry;

static const ReadCmdEntry g_readCmdTable[] =
{
    { CMD_READ_BLOCK,   g_buf10, BUF10_LEN },
    { CMD_READ_VERSION, g_buf13, BUF13_LEN },
    { CMD_READ_STATUS,  g_buf14, BUF14_LEN },
};
#define READ_CMD_TABLE_NUM  (sizeof(g_readCmdTable) / sizeof(g_readCmdTable[0]))

/*==========================================================================*/
/* RX(WRITE受信)側の状態機械                                                 */
/*==========================================================================*/

typedef enum
{
    RX_WAIT_CMD = 0,   /**< コマンドバイト待ち(各トランザクションの先頭) */
    RX_WAIT_LEN,       /**< LENバイト待ち(WRITE系コマンドのみ)           */
    RX_WAIT_PAYLOAD    /**< ペイロード受信中                              */
} RxState;

static RxState  g_rxState   = RX_WAIT_CMD;
static uint16_t g_rxTotal   = 0U;   /* 期待しているペイロード合計長(=LEN) */
static uint16_t g_rxIndex   = 0U;   /* 現在までの受信バイト数             */
static uint8_t *g_rxBuf     = (uint8_t *)0;  /* 格納先バッファ            */

/*==========================================================================*/
/* TX(READ応答)側のストリーム状態                                            */
/*==========================================================================*/

static const uint8_t *g_txBuf    = (const uint8_t *)0;
static uint16_t        g_txRemain = 0U;
static volatile uint16_t g_responseValid = 0U;  /* 1: 先入れ済みで送信中 */

#define RESP_DEFAULT_BUSY   (0xFFU)

/*==========================================================================*/
/* 初期化(起動時に1回呼ぶ)                                                   */
/*==========================================================================*/

/**
 * @brief I2Cプロトコル用バッファの初期化。I2C割り込みを有効化する前に呼ぶこと。
 * @param[in] pVersion    9byteのバージョン情報へのポインタ
 * @param[in] initStatus  起動直後のステータス値
 */
void I2C_Protocol_Init(const uint8_t *pVersion, uint8_t initStatus)
{
    uint16_t i;

    /* CMD 0x13 応答(バージョン+CRC)を起動時に一度だけ確定させる */
    for (i = 0U; i < (BUF13_LEN - 1U); i++)
    {
        g_buf13[i] = pVersion[i];
    }
    g_buf13[BUF13_LEN - 1U] = calcCRC8(g_buf13, BUF13_LEN - 1U);

    /* CMD 0x14 応答(ステータス+CRC)の初期値 */
    g_buf14[0] = initStatus;
    g_buf14[1] = calcCRC8(g_buf14, 1U);

    g_rxState        = RX_WAIT_CMD;
    g_responseValid  = 0U;
    g_txRemain       = 0U;
}

/**
 * @brief ステータス値(CMD0x14の応答)を更新する。運用中、状態が変わったタイミングで呼ぶ。
 * @param[in] newStatus 新しいステータス値
 */
void I2C_Protocol_UpdateStatus(uint8_t newStatus)
{
    g_buf14[0] = newStatus;
    g_buf14[1] = calcCRC8(g_buf14, 1U);
}

/*==========================================================================*/
/* I2C スレーブ割り込みハンドラ                                               */
/*==========================================================================*/

__interrupt void i2cSlaveISR(void)
{
    /*------------------------------------------------------------------
     * 1. アドレスマッチ(START/リピートスタート直後)
     *----------------------------------------------------------------*/
    if (I2caRegs.I2CSTR.bit.AAS == 1U)
    {
        if (I2caRegs.I2CSTR.bit.SDIR == 1U)
        {
            /* READ要求。本来はCMDバイト受信時に先入れ済みのはず。
             * 万一未確定なら、安全側のデフォルト値で応答する(最終防御)。
             */
            if (g_responseValid == 0U)
            {
                I2caRegs.I2CDXR = RESP_DEFAULT_BUSY;
                g_txBuf    = (const uint8_t *)0;
                g_txRemain = 0U;
            }
        }
        else
        {
            /* WRITE要求(新しいトランザクションの先頭) */
            g_rxState       = RX_WAIT_CMD;
            g_responseValid = 0U;
            g_txRemain      = 0U;
        }
    }

    /*------------------------------------------------------------------
     * 2. 受信データレディ
     *----------------------------------------------------------------*/
    if (I2caRegs.I2CSTR.bit.RRDY == 1U)
    {
        uint8_t  data = (uint8_t)I2caRegs.I2CDRR;
        uint16_t i;

        switch (g_rxState)
        {
            case RX_WAIT_CMD:
            {
                uint16_t handled = 0U;

                /* (a) READ系コマンドか? → 先入れする */
                for (i = 0U; i < READ_CMD_TABLE_NUM; i++)
                {
                    if (g_readCmdTable[i].cmd == data)
                    {
                        g_txBuf    = g_readCmdTable[i].pBuf;
                        g_txRemain = g_readCmdTable[i].totalLen;

                        /* ★先入れ: リピートスタートが来る前に最初の1byteを書く */
                        I2caRegs.I2CDXR = *g_txBuf;
                        g_txBuf++;
                        g_txRemain--;
                        g_responseValid = 1U;

                        handled = 1U;
                        break;
                    }
                }

                /* (b) WRITE系コマンドか? → LENバイト待ちへ */
                if (handled == 0U)
                {
                    for (i = 0U; i < WRITE_CMD_TABLE_NUM; i++)
                    {
                        if (g_writeCmdTable[i].cmd == data)
                        {
                            g_rxTotal = g_writeCmdTable[i].totalLen;
                            g_rxBuf   = g_writeCmdTable[i].pBuf;
                            g_rxState = RX_WAIT_LEN;
                            handled   = 1U;
                            break;
                        }
                    }
                }

                /* (c) 未知のコマンド: 何もしない(RX_WAIT_CMDのまま) */
                break;
            }

            case RX_WAIT_LEN:
            {
                if ((uint16_t)data == g_rxTotal)
                {
                    g_rxIndex = 0U;
                    g_rxState = RX_WAIT_PAYLOAD;
                }
                else
                {
                    /* LEN不一致: 異常とみなしコマンド待ちに戻す
                     * (必要に応じてエラーカウンタ等を増やす) */
                    g_rxState = RX_WAIT_CMD;
                }
                break;
            }

            case RX_WAIT_PAYLOAD:
            {
                /* データ部分・CRC部分とも、同じバッファへ順番に格納するだけでよい
                 * (バッファの末尾1byteが結果的にCRCの格納位置になる) */
                g_rxBuf[g_rxIndex] = data;
                g_rxIndex++;

                if (g_rxIndex >= g_rxTotal)
                {
                    /* 受信完了。必要であればここでCRCチェックを行う:
                     *
                     *   uint8_t calc = calcCRC8(g_rxBuf, g_rxTotal - 1U);
                     *   if (calc != g_rxBuf[g_rxTotal - 1U]) { エラー処理 }
                     */
                    g_rxState = RX_WAIT_CMD;
                }
                break;
            }

            default:
                g_rxState = RX_WAIT_CMD;
                break;
        }
    }

    /*------------------------------------------------------------------
     * 3. 送信データレディ(READ応答の2バイト目以降)
     *----------------------------------------------------------------*/
    if (I2caRegs.I2CSTR.bit.XRDY == 1U)
    {
        if (g_txRemain > 0U)
        {
            I2caRegs.I2CDXR = *g_txBuf;
            g_txBuf++;
            g_txRemain--;
        }
        else
        {
            /* 規定バイト数を超えてマスターが読み続けた場合のダミー応答 */
            I2caRegs.I2CDXR = RESP_DEFAULT_BUSY;
            g_responseValid = 0U;
        }
    }

    /*------------------------------------------------------------------
     * 4. STOP検出: トランザクション終了
     *----------------------------------------------------------------*/
    if (I2caRegs.I2CSTR.bit.SCD == 1U)
    {
        I2caRegs.I2CSTR.bit.SCD = 1U;   /* 1書き込みでフラグクリア */
        g_rxState        = RX_WAIT_CMD;
        g_responseValid  = 0U;
        g_txRemain       = 0U;
    }

    /* PIE割り込みフラグのクリア(使用グループ/ベクタに合わせて調整) */
    PieCtrlRegs.PIEACK.all |= PIEACK_GROUP8;
}
