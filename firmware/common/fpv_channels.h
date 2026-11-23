/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
/* Index order matches NODE_CMD_CHANNEL_INFO: A/B/E/F/R/DJI/Avatar/HDZero.
 * Names are tuning presets; equal frequencies cannot identify distinct pilots. */
static inline uint16_t fpv_channel_frequency(uint8_t channel){
    static const uint16_t frequencies[64]={
        5865,5845,5825,5805,5785,5765,5745,5725,
        5733,5752,5771,5790,5809,5828,5847,5866,
        5705,5685,5665,5645,5885,5905,5925,5945,
        5740,5760,5780,5800,5820,5840,5860,5880,
        5658,5695,5732,5769,5806,5843,5880,5917,
        5658,5695,5732,5769,5806,5839,5878,5917,
        5660,5695,5735,5770,5805,5840,5880,5915,
        5658,5695,5732,5769,5806,5843,5880,5917};
    return channel<64?frequencies[channel]:0;
}
