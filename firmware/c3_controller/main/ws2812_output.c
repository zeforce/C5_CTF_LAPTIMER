#include "ws2812_output.h"
#include "ws2812_frame.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>
#include <string.h>
static rmt_channel_handle_t channel;
static rmt_encoder_handle_t encoder;
static rmt_symbol_word_t *symbols;
static unsigned led_count;
static uint8_t intensity, previous[3];
static bool sent, inflight, completed;
static portMUX_TYPE completion_lock=portMUX_INITIALIZER_UNLOCKED;
static bool IRAM_ATTR tx_done(rmt_channel_handle_t ch,const rmt_tx_done_event_data_t *event,void *arg){
    (void)ch;(void)event;(void)arg;
    portENTER_CRITICAL_ISR(&completion_lock);completed=true;portEXIT_CRITICAL_ISR(&completion_lock);
    return false;
}
esp_err_t ws2812_output_init(int gpio,unsigned count,uint8_t brightness){
    if(gpio<0)return gpio==-1?ESP_OK:ESP_ERR_INVALID_ARG;
    if(channel)return ESP_ERR_INVALID_STATE;
    if(count<1 || count>32)return ESP_ERR_INVALID_ARG;
    symbols=calloc(count*24+1,sizeof(*symbols));if(!symbols)return ESP_ERR_NO_MEM;
    rmt_tx_channel_config_t cfg={.gpio_num=gpio,.clk_src=RMT_CLK_SRC_DEFAULT,.resolution_hz=10000000,.mem_block_symbols=48,.trans_queue_depth=1};
    esp_err_t e=rmt_new_tx_channel(&cfg,&channel);if(e!=ESP_OK)goto fail;
    rmt_copy_encoder_config_t enc={};e=rmt_new_copy_encoder(&enc,&encoder);if(e!=ESP_OK)goto fail;
    rmt_tx_event_callbacks_t cb={.on_trans_done=tx_done};
    e=rmt_tx_register_event_callbacks(channel,&cb,NULL);if(e!=ESP_OK)goto fail;
    e=rmt_enable(channel);if(e!=ESP_OK)goto fail;
    led_count=count;intensity=brightness;return ESP_OK;
fail:
    if(encoder){rmt_del_encoder(encoder);encoder=NULL;}
    if(channel){rmt_del_channel(channel);channel=NULL;}
    free(symbols);symbols=NULL;return e;
}
esp_err_t ws2812_output_update(uint32_t rgb,bool lit){
    if(!channel)return ESP_OK;
    if(inflight){
        portENTER_CRITICAL(&completion_lock);bool done=completed;portEXIT_CRITICAL(&completion_lock);
        if(!done)return ESP_OK;
        esp_err_t e=rmt_tx_wait_all_done(channel,0);if(e!=ESP_OK)return e;
        inflight=false;
    }
    uint8_t grb[3];ws2812_grb(rgb,intensity,lit,grb);
    if(sent && !memcmp(previous,grb,3))return ESP_OK;
    unsigned n=0;
    for(unsigned led=0;led<led_count;led++)for(unsigned byte=0;byte<3;byte++)for(int bit=7;bit>=0;bit--){
        bool one=(grb[byte]>>bit)&1;
        /* 10MHz: zero 0.4/0.8us, one 0.8/0.4us. */
        symbols[n++]=(rmt_symbol_word_t){.level0=1,.duration0=one?8:4,.level1=0,.duration1=one?4:8};
    }
    /* 300us low latch covers WS2812 variants requiring >280us reset. */
    symbols[n++]=(rmt_symbol_word_t){.level0=0,.duration0=1500,.level1=0,.duration1=1500};
    portENTER_CRITICAL(&completion_lock);completed=false;portEXIT_CRITICAL(&completion_lock);
    rmt_transmit_config_t tx={.flags.queue_nonblocking=1};
    esp_err_t e=rmt_transmit(channel,encoder,symbols,n*sizeof(*symbols),&tx);
    if(e==ESP_OK){inflight=true;sent=true;memcpy(previous,grb,3);}return e;
}

void ws2812_output_brightness(uint8_t brightness){intensity=brightness;}
