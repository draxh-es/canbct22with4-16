/*
 * pressure_node.c
 * BCT 22 (4-20 mA) basinc dugumu
 */

#include "pressure_node.h"
#include <stdbool.h>

#if (PN_TX_PERIOD_MS < PN_SAMPLE_PERIOD_MS)
#error "PN_TX_PERIOD_MS, PN_SAMPLE_PERIOD_MS'den kucuk olamaz"
#endif

volatile PN_Stats pn_stats;

static CAN_HandleTypeDef *s_hcan;
static ADC_HandleTypeDef *s_hadc;
static bool     s_ready;

static uint32_t s_t_sample;
static uint32_t s_t_tx;

static uint32_t s_acc_raw;      // ham ADC toplami
static uint32_t s_acc_n;        // toplam okuma sayisi
static float    s_i_ua;         // son akim
static int32_t  s_raw_mbar;     // darasiz son basinc
static int32_t  s_zero_mbar;    // dara
static uint8_t  s_flags;        // sonraki cerceveye eklenecek
static uint8_t  s_alive;


/* CAN */

// CubeMX'teki CAN ayari saatle uyumlu mu
static bool bitrate_ok(void)
{
    uint32_t btr = s_hcan->Instance->BTR;
    uint32_t brp = ((btr & CAN_BTR_BRP_Msk) >> CAN_BTR_BRP_Pos) + 1u;
    uint32_t ts1 = ((btr & CAN_BTR_TS1_Msk) >> CAN_BTR_TS1_Pos) + 1u;
    uint32_t ts2 = ((btr & CAN_BTR_TS2_Msk) >> CAN_BTR_TS2_Pos) + 1u;

    uint32_t rate = HAL_RCC_GetPCLK1Freq() / (brp * (1u + ts1 + ts2));
    uint32_t diff = (rate > PN_CAN_BITRATE) ? (rate - PN_CAN_BITRATE)
                                            : (PN_CAN_BITRATE - rate);

    return diff <= (PN_CAN_BITRATE / 200u);    // %0.5
}

// sadece komut ID'si alinir
static bool filter_setup(void)
{
    CAN_FilterTypeDef f = {0};

    f.FilterBank           = PN_CAN_FILTER_BANK;
    f.FilterMode           = CAN_FILTERMODE_IDLIST;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh         = PN_CAN_ID_COMMAND << 5;
    f.FilterIdLow          = 0;
    f.FilterMaskIdHigh     = PN_CAN_ID_COMMAND << 5;    // listede 2. ID, ayni
    f.FilterMaskIdLow      = 0;
    f.FilterFIFOAssignment = (PN_CAN_RX_FIFO == CAN_RX_FIFO0) ? CAN_FILTER_FIFO0
                                                              : CAN_FILTER_FIFO1;
    f.FilterActivation     = ENABLE;
    f.SlaveStartFilterBank = 14;

    return HAL_CAN_ConfigFilter(s_hcan, &f) == HAL_OK;
}

static void read_commands(void)
{
    CAN_RxHeaderTypeDef h;
    uint8_t d[8];

    while (HAL_CAN_GetRxFifoFillLevel(s_hcan, PN_CAN_RX_FIFO) > 0) {
        if (HAL_CAN_GetRxMessage(s_hcan, PN_CAN_RX_FIFO, &h, d) != HAL_OK)
            break;

        if (h.IDE != CAN_ID_STD || h.StdId != PN_CAN_ID_COMMAND || h.DLC < 1)
            continue;

        pn_stats.rx_cmd++;

        if (d[0] == PN_CMD_ZERO)
            s_zero_mbar = s_raw_mbar;
        else if (d[0] == PN_CMD_ZERO_CLEAR)
            s_zero_mbar = 0;
    }
}

static bool can_send(uint8_t d[8])
{
    CAN_TxHeaderTypeDef h = {0};
    uint32_t mailbox;

    h.StdId = PN_CAN_ID_PRESSURE;
    h.IDE   = CAN_ID_STD;
    h.RTR   = CAN_RTR_DATA;
    h.DLC   = 8;
    h.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_GetTxMailboxesFreeLevel(s_hcan) == 0)
        return false;    // 3 kutu da dolu

    return HAL_CAN_AddTxMessage(s_hcan, &h, d, &mailbox) == HAL_OK;
}


/* ADC */

// PN_ADC_OVERSAMPLE kez okuyup toplar
static bool adc_read_sum(uint32_t *sum)
{
    uint32_t s = 0;

    for (uint32_t i = 0; i < PN_ADC_OVERSAMPLE; i++) {
        if (HAL_ADC_Start(s_hadc) != HAL_OK)
            return false;
        if (HAL_ADC_PollForConversion(s_hadc, 2) != HAL_OK)
            return false;
        s += HAL_ADC_GetValue(s_hadc);
    }

    *sum = s;
    return true;
}

static void sample(void)
{
    uint32_t sum;

    if (adc_read_sum(&sum)) {
        s_acc_raw += sum;
        s_acc_n   += PN_ADC_OVERSAMPLE;
    } else {
        pn_stats.adc_fail++;
        s_flags |= PN_ST_ADC_FAIL;
    }
}


/* Gonderim */

static void send_frame(void)
{
    uint8_t st = s_flags;
    s_flags = 0;

    // periyottaki okumalarin ortalamasi
    if (s_acc_n > 0) {
        float raw = (float)s_acc_raw / (float)s_acc_n;
        float v   = raw * PN_VREF_V / 4095.0f;

        s_i_ua    = v / PN_SHUNT_OHM * 1.0e6f;
        s_acc_raw = 0;
        s_acc_n   = 0;
    } else {
        st |= PN_ST_ADC_FAIL;    // eski deger gider
    }

    // 4 mA ile 20 mA arasi dogrusal
    s_raw_mbar   = (int32_t)(PN_P_4MA_MBAR +
                   (s_i_ua - 4000.0f) * (PN_P_20MA_MBAR - PN_P_4MA_MBAR) / 16000.0f);
    int32_t mbar = s_raw_mbar - s_zero_mbar;

    if (s_i_ua < PN_I_OPEN_UA)                    st |= PN_ST_OPEN_LOOP;
    if (s_i_ua > PN_I_OVER_UA)                    st |= PN_ST_OVERRANGE;
    if (s_zero_mbar != 0)                         st |= PN_ST_ZERO_SET;
    if (__HAL_CAN_GET_FLAG(s_hcan, CAN_FLAG_EWG)) st |= PN_ST_CAN_WARN;

    uint16_t ua = (s_i_ua <= 0.0f)     ? 0u
                : (s_i_ua >= 65535.0f) ? 65535u
                : (uint16_t)s_i_ua;

    uint32_t p = (uint32_t)mbar;
    uint8_t d[8] = {
        (uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16), (uint8_t)(p >> 24),
        (uint8_t)ua, (uint8_t)(ua >> 8),
        st,
        s_alive++
    };

    if (can_send(d)) {
        pn_stats.tx_ok++;
    } else {
        pn_stats.tx_dropped++;
        s_flags |= PN_ST_TX_DROPPED;    // sonraki cercevede bildir
    }

    pn_stats.last_mbar = mbar;
    pn_stats.last_ua   = ua;
}


/* Disari acik fonksiyonlar */

PN_Status PN_Init(CAN_HandleTypeDef *hcan, ADC_HandleTypeDef *hadc)
{
    s_hcan  = hcan;
    s_hadc  = hadc;
    s_ready = false;

    if (!bitrate_ok())
        return PN_ERR_BITRATE;

    if (!filter_setup())
        return PN_ERR_FILTER;

    // baska modul baslattiysa tekrar baslatma
    if (HAL_CAN_GetState(s_hcan) != HAL_CAN_STATE_LISTENING) {
        if (HAL_CAN_Start(s_hcan) != HAL_OK)
            return PN_ERR_CAN_START;
    }

    s_t_sample = HAL_GetTick();
    s_t_tx     = s_t_sample;
    s_ready    = true;

    return PN_OK;
}

void PN_Process(void)
{
    if (!s_ready)
        return;

    uint32_t now = HAL_GetTick();

    read_commands();

    if (now - s_t_sample >= PN_SAMPLE_PERIOD_MS) {
        s_t_sample = now;
        sample();
    }

    if (now - s_t_tx >= PN_TX_PERIOD_MS) {
        s_t_tx = now;
        send_frame();
    }
}
