/* SPDX-License-Identifier: GPL-3.0-only */
#include "node_spi.h"
#include "node_spi_budget.h"
#include "sdkconfig.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define SLOT_BYTES 32u
#define HEADER_BYTES 8u
#define PIN_SCLK 4
#define PIN_MOSI 6
#define PIN_MISO 5
#define PIN_CS 7
#define PIN_READY 3
#define PIN_INT 10
#define CMD_NOP 0u
#define CMD_INFO 1u
#define CMD_RF_STATS 2u
#define CMD_GET_CONFIG 3u
#define CMD_SET_CONFIG 4u
#define CMD_OBSERVATION 5u
#define CMD_EVENT 6u
#define CMD_ACK_EVENT 7u
#define CMD_ACTIVE 8u
#define CMD_SCAN 9u
typedef struct {spi_device_handle_t device;int64_t deadline_us;uint8_t sequence;node_slot_pins_t pins;SemaphoreHandle_t mutex;node_spi_slot_stats_t stats;} node_spi_link_t;
static node_spi_link_t links[NODE_SPI_SLOTS];
static bool bus_initialized;
static SemaphoreHandle_t bus_mutex;
/* Legacy typed commands/diagnostics address slot0. */


static uint16_t crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0));
    }
    return crc;
}

static uint32_t read32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint16_t read16(const uint8_t *data) { return data[0] | ((uint16_t)data[1] << 8); }
static void put16(uint8_t *data, uint16_t value) { data[0] = value; data[1] = value >> 8; }
static void put32(uint8_t *data, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) data[i] = value >> (i * 8); }

bool node_spi_slot_enabled(unsigned index){return index<NODE_SPI_SLOTS && links[index].device!=NULL;}
bool node_spi_slot_ready(unsigned index){return node_spi_slot_enabled(index) && gpio_get_level(links[index].pins.ready)==1;}
bool node_spi_ready(void) { return node_spi_slot_ready(0); }
bool node_spi_interrupt(void) { return gpio_get_level(PIN_INT) == 1; }

static esp_err_t transfer_bits(node_spi_link_t *link,const uint8_t tx[SLOT_BYTES], uint8_t rx[SLOT_BYTES], unsigned bits)
{
    int64_t current=esp_timer_get_time();
    if(link->deadline_us && current>=link->deadline_us)return ESP_ERR_TIMEOUT;
    int64_t until = current + 2000000;
    if(link->deadline_us && until>link->deadline_us)until=link->deadline_us;
    while (!gpio_get_level(link->pins.ready) && esp_timer_get_time() < until) vTaskDelay(1);
    if (!gpio_get_level(link->pins.ready) || (link->deadline_us && esp_timer_get_time()>=link->deadline_us)) return ESP_ERR_TIMEOUT;
    /* Do not treat the old READY high as permission for the next slot. */
    spi_transaction_t transfer = {
        .length = bits,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(link->device, &transfer);
    if (err != ESP_OK) return err;
    until = esp_timer_get_time() + 2000000;
    if(link->deadline_us && until>link->deadline_us)until=link->deadline_us;
    while (gpio_get_level(link->pins.ready) && esp_timer_get_time() < until) esp_rom_delay_us(50);
    return gpio_get_level(link->pins.ready) ? ESP_ERR_TIMEOUT : ESP_OK;
}

static esp_err_t transfer_frame(node_spi_link_t *link,const uint8_t tx[SLOT_BYTES], uint8_t rx[SLOT_BYTES])
{
    return transfer_bits(link,tx, rx, SLOT_BYTES * 8);
}

static esp_err_t slot(node_spi_link_t *link,uint8_t command, uint8_t seq, const uint8_t *payload,
                      uint8_t length, uint8_t rx[SLOT_BYTES])
{
    if (length > 22) return ESP_ERR_INVALID_SIZE;
    uint8_t tx[SLOT_BYTES] = {'C', 'V', 1, command, seq, 0, length, 0};
    if (length) memcpy(tx + HEADER_BYTES, payload, length);
    uint16_t crc = crc16(tx, HEADER_BYTES + length);
    tx[HEADER_BYTES + length] = (uint8_t)crc;
    tx[HEADER_BYTES + length + 1] = (uint8_t)(crc >> 8);
    return transfer_frame(link,tx, rx);
}

/* The C5 response to a request appears in the following full-duplex slot. */
static esp_err_t request_sequence_link_locked(node_spi_link_t *link,uint8_t command, const uint8_t *tx_payload, uint8_t tx_length,
                         uint8_t expected_length, uint8_t *payload, uint8_t seq)
{
    if(node_spi_mutates(command))++link->stats.mutation_revision;
    uint8_t rx[SLOT_BYTES] = {0};
    esp_err_t err = slot(link,command, seq, tx_payload, tx_length, rx);
    if (err != ESP_OK) return err;
    memset(rx, 0, sizeof(rx));
    err = slot(link,CMD_NOP, ++link->sequence, NULL, 0, rx);
    if (err != ESP_OK) return err;
    if (rx[0] != 'C' || rx[1] != 'V' || rx[2] != 1 || rx[3] != command ||
        rx[4] != seq || rx[6] > 22 || rx[7] != 0) {
        if (++link->stats.invalid_responses <= 5)
            ESP_LOGW("node_spi", "bad response cmd=%u seq=%u hdr=%02x %02x %02x %02x %02x %02x %02x %02x",
                     command, seq, rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t actual_length = rx[6];
    uint16_t got = (uint16_t)rx[HEADER_BYTES + actual_length] |
                   ((uint16_t)rx[HEADER_BYTES + actual_length + 1] << 8);
    if (got != crc16(rx, HEADER_BYTES + actual_length)) return ESP_ERR_INVALID_CRC;
    if (rx[5] != 0) {
        ESP_LOGW("node_spi", "C5 rejected command %u with status %u", command, rx[5]);
        if (rx[5] == 8) return ESP_ERR_INVALID_ARG;
        if (rx[5] == 5) return ESP_ERR_INVALID_STATE;
        if (rx[5] == 4 || rx[5] == 1) return ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (actual_length != expected_length) return ESP_ERR_INVALID_SIZE;
    if (expected_length && payload) memcpy(payload, rx + HEADER_BYTES, expected_length);
    return ESP_OK;
}

/* Keep request plus delayed NOP response on the same endpoint. A busy bus
 * returns promptly so an absent/faulty receiver cannot stall its peer. */
static esp_err_t request_sequence_link(node_spi_link_t *link,uint8_t command,const uint8_t *input,uint8_t length,uint8_t expected,uint8_t *output,uint8_t seq){
 if(xSemaphoreTake(bus_mutex,0)!=pdTRUE)return ESP_ERR_TIMEOUT;
 esp_err_t e=request_sequence_link_locked(link,command,input,length,expected,output,seq);
 xSemaphoreGive(bus_mutex);return e;
}

static esp_err_t request_sequence(uint8_t command,const uint8_t *input,uint8_t length,uint8_t expected,uint8_t *output,uint8_t seq){
 node_spi_link_t *link=&links[0];if(!link->device)return ESP_ERR_INVALID_STATE;
 if(xSemaphoreTake(link->mutex,0)!=pdTRUE)return ESP_ERR_TIMEOUT;
 link->deadline_us=node_spi_default_deadline(command,esp_timer_get_time());
 esp_err_t e=request_sequence_link(link,command,input,length,expected,output,seq);link->deadline_us=0;++link->stats.requests;link->stats.last_error=e;if(e!=ESP_OK)++link->stats.errors;xSemaphoreGive(link->mutex);return e;
}
esp_err_t node_spi_exchange_until(unsigned index,uint8_t command,const uint8_t *input,uint8_t length,uint8_t expected,uint8_t *output,int64_t deadline_us){
 if(index>=NODE_SPI_SLOTS || length>22 || expected>22 || (length && !input) || (expected && !output) || deadline_us<0)return ESP_ERR_INVALID_ARG;
 node_spi_link_t *link=&links[index];if(!link->device)return ESP_ERR_INVALID_STATE;
 if(deadline_us && esp_timer_get_time()>=deadline_us)return ESP_ERR_TIMEOUT;
 if(xSemaphoreTake(link->mutex,deadline_us?0:pdMS_TO_TICKS(100))!=pdTRUE)return ESP_ERR_TIMEOUT;
 link->deadline_us=deadline_us?deadline_us:node_spi_default_deadline(command,esp_timer_get_time());
 esp_err_t e=request_sequence_link(link,command,input,length,expected,output,++link->sequence);
 link->deadline_us=0;++link->stats.requests;link->stats.last_error=e;if(e!=ESP_OK)++link->stats.errors;xSemaphoreGive(link->mutex);return e;
}
esp_err_t node_spi_exchange_slot(unsigned index,uint8_t command,const uint8_t *input,uint8_t length,uint8_t expected,uint8_t *output){
 return node_spi_exchange_until(index,command,input,length,expected,output,node_spi_default_deadline(command,esp_timer_get_time()));
}
bool node_spi_slot_stats(unsigned index,node_spi_slot_stats_t *out){
 if(index>=NODE_SPI_SLOTS || !out || !links[index].mutex || xSemaphoreTake(links[index].mutex,0)!=pdTRUE)return false;
 *out=links[index].stats;xSemaphoreGive(links[index].mutex);return true;
}

static esp_err_t request(uint8_t command, const uint8_t *input, uint8_t length,
                         uint8_t expected, uint8_t *output)
{
    return node_spi_exchange_slot(0,command,input,length,expected,output);
}

esp_err_t node_spi_attach(unsigned index,const node_slot_pins_t *pins){
 if(index>=NODE_SPI_SLOTS || !pins || !pins->enabled)return ESP_ERR_INVALID_ARG;
 if(!bus_initialized || links[index].device)return ESP_ERR_INVALID_STATE;
 node_slot_pins_t profile[NODE_SPI_SLOTS];for(unsigned i=0;i<NODE_SPI_SLOTS;i++)profile[i]=links[i].pins;profile[index]=*pins;
 if(!node_slot_pins_valid(profile))return ESP_ERR_INVALID_ARG;
 SemaphoreHandle_t mutex=xSemaphoreCreateMutex();if(!mutex)return ESP_ERR_NO_MEM;
 gpio_config_t inputs={.pin_bit_mask=(1ULL<<pins->ready)|(pins->interrupt>=0?1ULL<<pins->interrupt:0),.mode=GPIO_MODE_INPUT,.pull_down_en=GPIO_PULLDOWN_ENABLE,.intr_type=GPIO_INTR_DISABLE};
 esp_err_t e=gpio_config(&inputs);spi_device_handle_t device=NULL;
 if(e==ESP_OK){spi_device_interface_config_t cfg={.clock_speed_hz=100000,.mode=0,.spics_io_num=pins->cs,.queue_size=1};e=spi_bus_add_device(SPI2_HOST,&cfg,&device);}
 if(e!=ESP_OK){vSemaphoreDelete(mutex);return e;}
 links[index].device=device;links[index].pins=*pins;links[index].mutex=mutex;return ESP_OK;
}
esp_err_t node_spi_init(void){
 if(bus_initialized)return ESP_ERR_INVALID_STATE;
 bus_mutex=xSemaphoreCreateMutex();if(!bus_mutex)return ESP_ERR_NO_MEM;
 /* Deselect both receivers before configuring clocks or either device. */
 uint64_t cs_mask=1ULL<<PIN_CS;
#ifdef CONFIG_C3_DUAL_C5
 cs_mask|=1ULL<<0;
 gpio_set_level(0,1);
#endif
 gpio_set_level(PIN_CS,1);
 gpio_config_t cs_outputs={.pin_bit_mask=cs_mask,.mode=GPIO_MODE_OUTPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
 esp_err_t e=gpio_config(&cs_outputs);if(e!=ESP_OK)return e;
 spi_bus_config_t bus={.mosi_io_num=PIN_MOSI,.miso_io_num=PIN_MISO,.sclk_io_num=PIN_SCLK,.quadwp_io_num=-1,.quadhd_io_num=-1};
 e=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_DISABLED);if(e!=ESP_OK)return e;bus_initialized=true;
 node_slot_pins_t pins={.cs=PIN_CS,.ready=PIN_READY,.interrupt=PIN_INT,.enabled=true};
 e=node_spi_attach(0,&pins);if(e!=ESP_OK)return e;
#ifdef CONFIG_C3_DUAL_C5
 pins=(node_slot_pins_t){.cs=0,.ready=1,.interrupt=-1,.enabled=true};
 e=node_spi_attach(1,&pins);if(e!=ESP_OK)return e;
 ESP_LOGI("node_spi","Dual C5: slot0 CS=7 READY=3 INT=10; slot1 CS=0 READY=1 INT=polled");
#endif
 return ESP_OK;
}

esp_err_t node_spi_exchange(uint8_t command, const uint8_t *input, uint8_t input_length,
    uint8_t expected_length, uint8_t *output)
{
    if (input_length > 22 || expected_length > 22 || (input_length && !input)) return ESP_ERR_INVALID_ARG;
    return request(command, input, input_length, expected_length, output);
}

esp_err_t node_spi_get_info(node_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t data[17];
    esp_err_t err = request(CMD_INFO, NULL, 0, sizeof(data), data);
    if (err != ESP_OK) return err;
    memcpy(out->mac, data, 6);
    out->session = read32(data + 6);
    out->channel = data[10]; out->rf_valid = data[11]; out->faults = data[12];
    out->transactions = read32(data + 13);
    return ESP_OK;
}

esp_err_t node_spi_get_rf_stats(node_rf_stats_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t data[20];
    esp_err_t err = request(CMD_RF_STATS, NULL, 0, sizeof(data), data);
    if (err != ESP_OK) return err;
    out->windows = read32(data);
    out->valid_windows = read32(data + 4);
    out->receive_errors = read32(data + 8);
    out->raw_power = read32(data + 12);
    out->clip_pm = read32(data + 16);
    return ESP_OK;
}

esp_err_t node_spi_get_config(node_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t data[8];
    esp_err_t err = request(CMD_GET_CONFIG, NULL, 0, sizeof(data), data);
    if (err == ESP_OK) {
        out->channel = data[0]; out->gain = data[1];
        out->enter_power = read16(data + 2); out->exit_power = read16(data + 4);
        out->confirm_ms = read16(data + 6);
    }
    return err;
}

esp_err_t node_spi_set_config(const node_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    uint8_t data[8] = {cfg->channel, cfg->gain};
    put16(data + 2, cfg->enter_power); put16(data + 4, cfg->exit_power);
    put16(data + 6, cfg->confirm_ms);
    return request(CMD_SET_CONFIG, data, sizeof(data), 0, NULL);
}

esp_err_t node_spi_get_observation(node_observation_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t data[16];
    esp_err_t err = request(CMD_OBSERVATION, NULL, 0, sizeof(data), data);
    if (err == ESP_OK) {
        out->power = read16(data); out->clip_pm = read16(data + 2);
        out->sample_count = read32(data + 4); out->overflow_count = read32(data + 8);
        out->queue_depth = data[12]; out->active = data[13]; out->calibrated = data[14];
    }
    return err;
}

esp_err_t node_spi_get_event(node_event_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t data[18];
    esp_err_t err = request(CMD_EVENT, NULL, 0, sizeof(data), data);
    if (err == ESP_OK) {
        out->sequence = read32(data);
        out->timestamp_us = (uint64_t)read32(data + 4) | ((uint64_t)read32(data + 8) << 32);
        out->power = read16(data + 12); out->clip_pm = read16(data + 14);
        out->edge = data[16]; out->valid = data[17];
    }
    return err;
}

esp_err_t node_spi_ack_event(uint32_t sequence)
{
    uint8_t data[4]; put32(data, sequence);
    return request(CMD_ACK_EVENT, data, sizeof(data), 0, NULL);
}

esp_err_t node_spi_set_active(bool active, bool calibrated)
{
    uint8_t data[2] = {active, calibrated};
    return request(CMD_ACTIVE, data, sizeof(data), 0, NULL);
}

esp_err_t node_spi_scan(uint8_t start, uint16_t power[8])
{
    if (!power || start >= 64 || start % 8) return ESP_ERR_INVALID_ARG;
    uint8_t data[16];
    esp_err_t err = request(CMD_SCAN, &start, 1, sizeof(data), data);
    if (err == ESP_OK) for (unsigned i = 0; i < 8; ++i) power[i] = read16(data + i * 2);
    return err;
}

esp_err_t node_spi_get_capabilities(node_capabilities_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t data[22];
    esp_err_t err = request(NODE_CMD_CAPABILITIES, NULL, 0, sizeof(data), data);
    if (err != ESP_OK) return err;
    if (data[0] != 1) return ESP_ERR_NOT_SUPPORTED;
    *out = (node_capabilities_t){data[0], data[1], data[2], data[3],
        data[4], data[5], data[6], data[7], read32(data + 8),
        read16(data + 12), read16(data + 14), read16(data + 16), read16(data + 18), read16(data + 20)};
    return ESP_OK;
}

esp_err_t node_spi_get_clock(node_clock_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t data[12];
    esp_err_t err = request(NODE_CMD_CLOCK, NULL, 0, sizeof(data), data);
    if (err == ESP_OK) {
        out->timestamp_us = (uint64_t)read32(data) | ((uint64_t)read32(data + 4) << 32);
        out->session = read32(data + 8);
    }
    return err;
}

esp_err_t node_spi_get_observation_detail(node_observation_detail_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t data[22];
    esp_err_t err = request(NODE_CMD_OBSERVATION_DETAIL, NULL, 0, sizeof(data), data);
    if (err == ESP_OK) {
        out->timestamp_us = (uint64_t)read32(data) | ((uint64_t)read32(data + 4) << 32);
        out->power = read16(data + 8); out->clip_pm = read16(data + 10);
        out->sample_count = read32(data + 12); out->session = read32(data + 16);
        out->quality_flags = data[20]; out->channel = data[21];
    }
    return err;
}

/* Raw malformed frame and its NOP response must stay together under the link
 * mutex. All READY waits share the same 100ms budget. */
static esp_err_t diagnostic_raw(bool short_frame,unsigned index,const uint8_t config[8]) {
 node_spi_link_t *link=&links[0];
 if(!link->device)return ESP_ERR_INVALID_STATE;
 if(xSemaphoreTake(link->mutex,0)!=pdTRUE)return ESP_ERR_TIMEOUT;
 link->deadline_us=esp_timer_get_time()+100000;
 uint8_t seq=++link->sequence,tx[32]={'C','V',1,CMD_SET_CONFIG,seq,0,8,0},rx[32];
 memcpy(tx+8,config,8);tx[9]=(config[1]+1)%64;
 const uint8_t lengths[]={1,7,8,16,18,31},expected[]={3,1,2,2,2,4};
 if(!short_frame){
  if(index==1)tx[2]=99;
  if(index==2)tx[6]=23;
  if(index==3)tx[5]=1;
  if(index==4)tx[7]=1;
  if(index==5)tx[0]='X';
 }
 if(tx[6]<=22)put16(tx+8+tx[6],crc16(tx,8+tx[6]));
 if(!short_frame && !index)tx[16]^=1;
 esp_err_t e=transfer_bits(link,tx,rx,short_frame?lengths[index]*8:256);
 if(!e)e=slot(link,CMD_NOP,++link->sequence,NULL,0,rx);
 if(!e && !(rx[0]=='C' && rx[1]=='V' && rx[2]==1 &&
   rx[3]==(short_frame?CMD_NOP:CMD_SET_CONFIG) && rx[4]==(short_frame?0:seq) &&
   rx[5]==(short_frame?2:expected[index]) && !rx[6] && !rx[7] && read16(rx+8)==crc16(rx,8)))e=ESP_ERR_INVALID_RESPONSE;
 link->deadline_us=0;++link->stats.requests;link->stats.last_error=e;if(e)++link->stats.errors;
 xSemaphoreGive(link->mutex);return e;
}
esp_err_t node_spi_protocol_step(node_spi_protocol_test_t *t,bool *done){
 if(!t || !done || t->kind>2 || t->step>14)return ESP_ERR_INVALID_ARG;
 *done=false;uint8_t data[22];esp_err_t e;
 if(t->kind==2){
  const uint8_t range[2]={0,64};
  switch(t->step){
   case 0:t->sequence=++links[0].sequence;e=request_sequence(NODE_CMD_SCAN_START,range,2,0,NULL,t->sequence);break;
   case 1:e=request(NODE_CMD_SCAN_STATUS,NULL,0,12,t->health);break;
   case 2:e=request_sequence(NODE_CMD_SCAN_START,range,2,0,NULL,t->sequence);break;
   case 3:
    e=request(NODE_CMD_SCAN_STATUS,NULL,0,12,data);
    if(!e && (t->health[0]!=1 || data[0]!=1 || memcmp(t->health+8,data+8,4) || data[1]<t->health[1]))e=ESP_ERR_INVALID_RESPONSE;
    *done=true;break;
   default:return ESP_ERR_INVALID_STATE;
  }
  if(!e)++t->step;
  return e;
 }
 if(t->step==0)e=request(CMD_GET_CONFIG,NULL,0,8,t->config);
 else if(t->step==1)e=request(NODE_CMD_HEALTH,NULL,0,22,t->health);
 else if(t->step==14){
  e=request(NODE_CMD_HEALTH,NULL,0,22,data);
  if(!e && read32(data+12)-read32(t->health+12)!=6)e=ESP_ERR_INVALID_RESPONSE;
  *done=true;
 }else if(!(t->step&1))e=diagnostic_raw(t->kind,(t->step-2)/2,t->config);
 else{
  e=request(CMD_GET_CONFIG,NULL,0,8,data);
  if(!e && memcmp(data,t->config,8))e=ESP_ERR_INVALID_RESPONSE;
  if(!e)ESP_LOGI("node_spi","PROTOCOL case=%u short=%u config_unchanged=1",(t->step-3)/2,t->kind);
 }
 if(!e)++t->step;
 return e;
}
