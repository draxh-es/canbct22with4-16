/*
 * pressure_node.h
 * BCT 22 (4-20 mA) basinc dugumu
 * Detaylar: README.md
 */

#ifndef PRESSURE_NODE_H
#define PRESSURE_NODE_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

/* CAN ID - son duzenleyen kisi belirler */
#define PN_CAN_ID_PRESSURE    0x120u      // ornek
#define PN_CAN_ID_COMMAND     0x121u      // ornek

/* Hizlar */
#define PN_CAN_BITRATE        500000u     // CubeMX ile ayni olmali
#define PN_SAMPLE_PERIOD_MS   1u          // olcum 1 kHz
#define PN_TX_PERIOD_MS       10u         // 10: 100 Hz normal, 1: 1 kHz hizli

/* Sensor ve on uc */
#define PN_P_4MA_MBAR         0.0f        // 4 mA'deki basinc
#define PN_P_20MA_MBAR        100000.0f   // 20 mA'deki basinc, siparis koduna gore
#define PN_SHUNT_OHM          150.0f
#define PN_VREF_V             3.3f        // kartta VDDA olcup yaz
#define PN_ADC_OVERSAMPLE     16u

/* Akim sinirlari */
#define PN_I_OPEN_UA          3600.0f     // alti: hat kopuk
#define PN_I_OVER_UA          20500.0f    // ustu: aralik disi

/* Ortak kaynaklar, baska modul kullanmasin */
#define PN_CAN_FILTER_BANK    0u
#define PN_CAN_RX_FIFO        CAN_RX_FIFO1

/* Durum bitleri (bayt 6) */
#define PN_ST_OPEN_LOOP       (1u << 0)
#define PN_ST_OVERRANGE       (1u << 1)
#define PN_ST_CAN_WARN        (1u << 2)
#define PN_ST_ZERO_SET        (1u << 3)
#define PN_ST_TX_DROPPED      (1u << 4)
#define PN_ST_ADC_FAIL        (1u << 5)

/* Komutlar (bayt 0) */
#define PN_CMD_ZERO           0x01u       // dara al
#define PN_CMD_ZERO_CLEAR     0x02u       // darayi sil

typedef enum {
    PN_OK = 0,
    PN_ERR_BITRATE,       // saat ayari CAN hizini bozmus
    PN_ERR_FILTER,
    PN_ERR_CAN_START      // transceiver veya hat sorunu
} PN_Status;

typedef struct {
    uint32_t tx_ok;
    uint32_t tx_dropped;
    uint32_t rx_cmd;
    uint32_t adc_fail;
    int32_t  last_mbar;
    uint16_t last_ua;
} PN_Stats;

extern volatile PN_Stats pn_stats;        // Live Expressions ile izle

PN_Status PN_Init(CAN_HandleTypeDef *hcan, ADC_HandleTypeDef *hadc);
void      PN_Process(void);               // while(1) icinde cagir

#endif
