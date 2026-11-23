#include "ble_control.h"
#include "io_service.h"
#include "feedback.h"
#include "finder.h"
#include "ws2812_output.h"
#include "controller_state.h"
#include "network.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <inttypes.h>
#include <string.h>
#include "nvs.h"
static feedback_t engine;
static feedback_output_t output;
static finder_t finder;
static QueueHandle_t buttons;
static portMUX_TYPE io_lock=portMUX_INITIALIZER_UNLOCKED;
static uint32_t dropped;
static uint32_t ws_errors;
static esp_err_t ws_error,save_error;
static uint8_t preferences[4]={1,1,1,CONFIG_C3_FEEDBACK_WS2812_BRIGHTNESS};
esp_err_t io_service_configure(bool light,bool sound,uint8_t brightness){
 uint8_t next[4]={1,light,sound,brightness};
 nvs_handle_t h;esp_err_t e=nvs_open("feedback",NVS_READWRITE,&h);
 if(e==ESP_OK){e=nvs_set_blob(h,"settings",next,sizeof(next));if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}
 taskENTER_CRITICAL(&io_lock);save_error=e;if(e==ESP_OK)memcpy(preferences,next,sizeof(next));taskEXIT_CRITICAL(&io_lock);
 return e;
}
void io_service_status(uint8_t out[32]){
 memset(out,0,32);out[0]=13;
 taskENTER_CRITICAL(&io_lock);
 memcpy(out+1,preferences+1,3);memcpy(out+8,&save_error,4);memcpy(out+12,&ws_error,4);memcpy(out+16,&ws_errors,4);
 memcpy(out+20,&engine.transitions,4);memcpy(out+24,&engine.clicks,4);memcpy(out+28,&engine.holds,4);
 taskEXIT_CRITICAL(&io_lock);
 out[4]=CONFIG_C3_FEEDBACK_LED_GPIO+1;out[5]=CONFIG_C3_FEEDBACK_BUZZER_GPIO+1;out[6]=CONFIG_C3_FEEDBACK_BUTTON_GPIO+1;out[7]=CONFIG_C3_FEEDBACK_WS2812_GPIO+1;
}
void io_service_finder_status(uint8_t out[32]){
 memset(out,0,32);out[0]=30;out[1]=controller_finder_active();
 controller_snapshot_t s;controller_get_snapshot(&s);out[2]=s.channel;out[3]=s.gain;
 taskENTER_CRITICAL(&io_lock);out[4]=finder.valid;out[5]=finder.level;out[6]=finder.trend;memcpy(out+8,&finder.power,2);taskEXIT_CRITICAL(&io_lock);
}
/* No output pin is selected without an explicit board mapping. */
static bool allowed(int pin,bool input){
 if(pin==-1)return true;
 if(pin<0 || pin>21 || (input?!GPIO_IS_VALID_GPIO(pin):!GPIO_IS_VALID_OUTPUT_GPIO(pin)))return false;
#ifdef CONFIG_C3_DUAL_C5
 if(pin==0 || pin==1)return false;
 /* USB owns the console; UART0 pins are available for feedback. */
 if(pin==20 || pin==21)return true;
#endif
 switch(pin){case 2:case 3:case 4:case 5:case 6:case 7:case 8:case 9:case 10:case 11:case 12:case 13:case 14:case 15:case 16:case 17:case 18:case 19:case 20:case 21:return false;default:return true;}
}
static void button_worker(void *arg){
 (void)arg;uint8_t event;
 for(;;)if(xQueueReceive(buttons,&event,portMAX_DELAY)==pdTRUE){
  if(controller_finder_active()){
   if(event==FEEDBACK_HOLD)controller_finder(false,0);
   continue;
  }
  uint8_t state,owner,fault;if(!controller_game_feedback(&state,&owner,&fault))continue;
  if(state==0 || state==3){
   if(event==FEEDBACK_HOLD){esp_err_t e=ble_control_open_authorization();ESP_LOGI("io","BLE local authorization: %s",esp_err_to_name(e));}
   continue;
  }
  /* Members never become an alternate control source. A local long press
   * remains an emergency stop, while host actions use the network entry. */
  uint8_t command[3]={1,10,event==FEEDBACK_HOLD?0:3};
  esp_err_t error=network_role()==NET_MEMBER?controller_game_action(command[2]):network_control(command,3);
  ESP_LOGI("io","BUTTON event=%u action=%u result=%s",event,command[2],esp_err_to_name(error));
 }
}
static void io_worker(void *arg){
 (void)arg;
 feedback_input_t input={0};TickType_t wake=xTaskGetTickCount();
 for(;;){
  controller_snapshot_t snapshot;controller_get_snapshot(&snapshot);input.rf_online=snapshot.node_online;
  controller_feedback_read(&input);
  uint8_t schedule[32];network_status(schedule,45);
  input.countdown_phase=schedule[1];memcpy(&input.countdown_ms,schedule+20,4);
  if(input.state==0)input.laps=snapshot.total_laps;
  bool pressed=CONFIG_C3_FEEDBACK_BUTTON_GPIO>=0 && gpio_get_level(CONFIG_C3_FEEDBACK_BUTTON_GPIO)==0;
  taskENTER_CRITICAL(&io_lock);output=feedback_tick(&engine,&input,pressed,esp_timer_get_time()/1000);feedback_output_t current=output;uint8_t light=preferences[1],sound=preferences[2],brightness=preferences[3];taskEXIT_CRITICAL(&io_lock);
  bool finding=controller_finder_active();uint64_t now=esp_timer_get_time();
  /* No calibration required. Reject transport/settling/stale observations,
   * but constant power and clipping still carry useful search information. */
  bool valid=finding && snapshot.node_online && snapshot.rf_valid && snapshot.observation_samples &&
   now>=snapshot.observation_updated_us && now-snapshot.observation_updated_us<1500000 &&
   !(snapshot.observation_quality&(NODE_QUALITY_SETTLING|NODE_QUALITY_NO_SAMPLE|NODE_QUALITY_STALE));
  taskENTER_CRITICAL(&io_lock);finder_update(&finder,valid,snapshot.observed_power,snapshot.observation_updated_us);
  if(finding){current.led=finder.valid?(finder.power?finder_pulse(&finder,now/1000):now/1000%1200<60):now/1000%500<250;
   current.rgb=!finder.valid?0xff8000:finder.trend==1?0x00ff00:finder.trend==2?0xff0000:0x0080ff;
   current.buzzer=finder_pulse(&finder,now/1000);}
  taskEXIT_CRITICAL(&io_lock);
  current.led&=light;current.buzzer&=sound;ws2812_output_brightness(brightness);
  if(CONFIG_C3_FEEDBACK_LED_GPIO>=0)gpio_set_level(CONFIG_C3_FEEDBACK_LED_GPIO,current.led);
  if(CONFIG_C3_FEEDBACK_BUZZER_GPIO>=0)gpio_set_level(CONFIG_C3_FEEDBACK_BUZZER_GPIO,current.buzzer);
  esp_err_t e=ws2812_output_update(current.rgb,current.led);
  taskENTER_CRITICAL(&io_lock);ws_error=e;if(e!=ESP_OK)++ws_errors;taskEXIT_CRITICAL(&io_lock);
  if(current.button_event && xQueueSend(buttons,&current.button_event,0)!=pdTRUE)++dropped;
  vTaskDelayUntil(&wake,pdMS_TO_TICKS(20));
 }
}
void io_service_diagnostics(void){
 taskENTER_CRITICAL(&io_lock);feedback_t f=engine;feedback_output_t o=output;uint32_t errors=ws_errors;esp_err_t error=ws_error;taskEXIT_CRITICAL(&io_lock);
 ESP_LOGI("io","WS2812 gpio=%d count=%d brightness=%d error=%s errors=%"PRIu32,CONFIG_C3_FEEDBACK_WS2812_GPIO,CONFIG_C3_FEEDBACK_WS2812_COUNT,CONFIG_C3_FEEDBACK_WS2812_BRIGHTNESS,esp_err_to_name(error),errors);
 ESP_LOGI("io","IO led_gpio=%d buzzer_gpio=%d button_gpio=%d rgb=%06"PRIx32" led=%u buzzer=%u transitions=%"PRIu32" clicks=%"PRIu32" holds=%"PRIu32" dropped=%"PRIu32,CONFIG_C3_FEEDBACK_LED_GPIO,CONFIG_C3_FEEDBACK_BUZZER_GPIO,CONFIG_C3_FEEDBACK_BUTTON_GPIO,o.rgb,o.led,o.buzzer,f.transitions,f.clicks,f.holds,dropped);
}
esp_err_t io_service_start(void){
 nvs_handle_t h;esp_err_t load=nvs_open("feedback",NVS_READONLY,&h);
 if(load==ESP_OK){uint8_t saved[4];size_t size=sizeof(saved);load=nvs_get_blob(h,"settings",saved,&size);nvs_close(h);if(load==ESP_OK){if(size==4 && saved[0]==1 && saved[1]<=1 && saved[2]<=1)memcpy(preferences,saved,4);else load=ESP_ERR_INVALID_RESPONSE;}}
 if(load!=ESP_OK && load!=ESP_ERR_NVS_NOT_FOUND)save_error=load;

 int pins[4]={CONFIG_C3_FEEDBACK_LED_GPIO,CONFIG_C3_FEEDBACK_BUZZER_GPIO,CONFIG_C3_FEEDBACK_BUTTON_GPIO,CONFIG_C3_FEEDBACK_WS2812_GPIO};
 for(unsigned i=0;i<4;i++){
  if(!allowed(pins[i],i==2))return ESP_ERR_INVALID_ARG;
  for(unsigned j=0;j<i;j++)if(pins[i]>=0 && pins[i]==pins[j])return ESP_ERR_INVALID_ARG;
 }
 esp_err_t strip=ws2812_output_init(pins[3],CONFIG_C3_FEEDBACK_WS2812_COUNT,CONFIG_C3_FEEDBACK_WS2812_BRIGHTNESS);if(strip!=ESP_OK)return strip;
 for(unsigned i=0;i<3;i++)if(pins[i]>=0){gpio_config_t cfg={.pin_bit_mask=1ULL<<pins[i],.mode=i==2?GPIO_MODE_INPUT:GPIO_MODE_OUTPUT,.pull_up_en=i==2};esp_err_t e=gpio_config(&cfg);if(e!=ESP_OK)return e;}
 feedback_init(&engine);buttons=xQueueCreate(8,1);if(!buttons)return ESP_ERR_NO_MEM;
 if(xTaskCreate(button_worker,"io_button",4096,NULL,2,NULL)!=pdPASS)return ESP_ERR_NO_MEM;
 return xTaskCreate(io_worker,"io_feedback",4096,NULL,2,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
