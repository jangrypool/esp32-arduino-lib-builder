/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "riscv/interrupt.h"
#include "riscv/riscv_interrupts.h"
#include "esp_types.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_intr_alloc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_private/wifi_os_adapter.h"
#include "esp_private/wifi.h"
#include "esp_phy_init.h"
#include "esp32c3/clk.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/rtc.h"
#include "soc/syscon_reg.h"
#include "soc/system_reg.h"
#include "phy_init_data.h"
#include "driver/periph_ctrl.h"
#include "nvs.h"
#include "os.h"
#include "esp_smartconfig.h"
#include "esp_coexist_internal.h"
#include "esp_coexist_adapter.h"

#define TAG "esp_adapter"

#define MHZ (1000000)

#ifdef CONFIG_PM_ENABLE
extern void wifi_apb80m_request(void);
extern void wifi_apb80m_release(void);
#endif

IRAM_ATTR void *wifi_malloc( size_t size )
{
    return malloc(size);
}

IRAM_ATTR void *wifi_realloc( void *ptr, size_t size )
{
    return realloc(ptr, size);
}

IRAM_ATTR void *wifi_calloc( size_t n, size_t size )
{
    return calloc(n, size);
}

static void * IRAM_ATTR wifi_zalloc_wrapper(size_t size)
{
    void *ptr = wifi_calloc(1, size);
    return ptr;
}

wifi_static_queue_t* wifi_create_queue( int queue_len, int item_size)
{
    wifi_static_queue_t *queue = NULL;

    queue = (wifi_static_queue_t*)heap_caps_malloc(sizeof(wifi_static_queue_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!queue) {
        return NULL;
    }

    queue->handle = xQueueCreate( queue_len, item_size);
    return queue;
}

void wifi_delete_queue(wifi_static_queue_t *queue)
{
    if (queue) {
        vQueueDelete(queue->handle);
        free(queue);
    }
}

static void * wifi_create_queue_wrapper(int queue_len, int item_size)
{
    return wifi_create_queue(queue_len, item_size);
}

static void wifi_delete_queue_wrapper(void *queue)
{
    wifi_delete_queue(queue);
}

static bool IRAM_ATTR env_is_chip_wrapper(void)
{
#ifdef CONFIG_IDF_ENV_FPGA
    return false;
#else
    return true;
#endif
}

static void set_intr_wrapper(int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio)
{
    intr_matrix_route(intr_source, intr_num);
    esprv_intc_int_set_priority(intr_num, intr_prio);
    esprv_intc_int_set_type(intr_num, INTR_TYPE_LEVEL);
}

static void clear_intr_wrapper(uint32_t intr_source, uint32_t intr_num)
{

}

static void set_isr_wrapper(int32_t n, void *f, void *arg)
{
    intr_handler_set(n, (intr_handler_t)f, arg);
}

static void enable_intr_wrapper(uint32_t intr_mask)
{
    esprv_intc_int_enable(intr_mask);
}

static void disable_intr_wrapper(uint32_t intr_mask)
{
    esprv_intc_int_disable(intr_mask);
}

static void * spin_lock_create_wrapper(void)
{
    portMUX_TYPE tmp = portMUX_INITIALIZER_UNLOCKED;
    void *mux = malloc(sizeof(portMUX_TYPE));

    if (mux) {
        memcpy(mux,&tmp,sizeof(portMUX_TYPE));
        return mux;
    }
    return NULL;
}

static uint32_t IRAM_ATTR wifi_int_disable_wrapper(void *wifi_int_mux)
{
    if (xPortInIsrContext()) {
        portENTER_CRITICAL_ISR(wifi_int_mux);
    } else {
        portENTER_CRITICAL(wifi_int_mux);
    }

    return 0;
}

static void IRAM_ATTR wifi_int_restore_wrapper(void *wifi_int_mux, uint32_t tmp)
{
    if (xPortInIsrContext()) {
        portEXIT_CRITICAL_ISR(wifi_int_mux);
    } else {
        portEXIT_CRITICAL(wifi_int_mux);
    }
}

static bool IRAM_ATTR is_from_isr_wrapper(void)
{
    return !xPortCanYield();
}

static void IRAM_ATTR task_yield_from_isr_wrapper(void)
{
    portYIELD_FROM_ISR();
}

static void * semphr_create_wrapper(uint32_t max, uint32_t init)
{
    return (void *)xSemaphoreCreateCounting(max, init);
}

static void semphr_delete_wrapper(void *semphr)
{
    vSemaphoreDelete(semphr);
}

static void wifi_thread_semphr_free(void* data)
{
    SemaphoreHandle_t *sem = (SemaphoreHandle_t*)(data);

    if (sem) {
        vSemaphoreDelete(sem);
    }
}

static void * wifi_thread_semphr_get_wrapper(void)
{
    static bool s_wifi_thread_sem_key_init = false;
    static pthread_key_t s_wifi_thread_sem_key;
    SemaphoreHandle_t sem = NULL;

    if (s_wifi_thread_sem_key_init == false) {
        if (0 != pthread_key_create(&s_wifi_thread_sem_key, wifi_thread_semphr_free)) {
            return NULL;
        }
        s_wifi_thread_sem_key_init = true;
    }

    sem = pthread_getspecific(s_wifi_thread_sem_key);
    if (!sem) {
        sem = xSemaphoreCreateCounting(1, 0);
        if (sem) {
            pthread_setspecific(s_wifi_thread_sem_key, sem);
            ESP_LOGV(TAG, "thread sem create: sem=%p", sem);
        }
    }

    ESP_LOGV(TAG, "thread sem get: sem=%p", sem);
    return (void*)sem;
}

static int32_t IRAM_ATTR semphr_take_from_isr_wrapper(void *semphr, void *hptw)
{
    return (int32_t)xSemaphoreTakeFromISR(semphr, hptw);
}

static int32_t IRAM_ATTR semphr_give_from_isr_wrapper(void *semphr, void *hptw)
{
    return (int32_t)xSemaphoreGiveFromISR(semphr, hptw);
}

static int32_t semphr_take_wrapper(void *semphr, uint32_t block_time_tick)
{
    if (block_time_tick == OSI_FUNCS_TIME_BLOCKING) {
        return (int32_t)xSemaphoreTake(semphr, portMAX_DELAY);
    } else {
        return (int32_t)xSemaphoreTake(semphr, block_time_tick);
    }
}

static int32_t semphr_give_wrapper(void *semphr)
{
    return (int32_t)xSemaphoreGive(semphr);
}

static void * recursive_mutex_create_wrapper(void)
{
    return (void *)xSemaphoreCreateRecursiveMutex();
}

static void * mutex_create_wrapper(void)
{
    return (void *)xSemaphoreCreateMutex();
}

static void mutex_delete_wrapper(void *mutex)
{
    vSemaphoreDelete(mutex);
}

static int32_t IRAM_ATTR mutex_lock_wrapper(void *mutex)
{
    return (int32_t)xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
}

static int32_t IRAM_ATTR mutex_unlock_wrapper(void *mutex)
{
    return (int32_t)xSemaphoreGiveRecursive(mutex);
}

static void * queue_create_wrapper(uint32_t queue_len, uint32_t item_size)
{
    return (void *)xQueueCreate(queue_len, item_size);
}

static int32_t queue_send_wrapper(void *queue, void *item, uint32_t block_time_tick)
{
    if (block_time_tick == OSI_FUNCS_TIME_BLOCKING) {
        return (int32_t)xQueueSend(queue, item, portMAX_DELAY);
    } else {
        return (int32_t)xQueueSend(queue, item, block_time_tick);
    }
}

static int32_t IRAM_ATTR queue_send_from_isr_wrapper(void *queue, void *item, void *hptw)
{
    return (int32_t)xQueueSendFromISR(queue, item, hptw);
}

static int32_t queue_send_to_back_wrapper(void *queue, void *item, uint32_t block_time_tick)
{
    return (int32_t)xQueueGenericSend(queue, item, block_time_tick, queueSEND_TO_BACK);
}

static int32_t queue_send_to_front_wrapper(void *queue, void *item, uint32_t block_time_tick)
{
    return (int32_t)xQueueGenericSend(queue, item, block_time_tick, queueSEND_TO_FRONT);
}

static int32_t queue_recv_wrapper(void *queue, void *item, uint32_t block_time_tick)
{
    if (block_time_tick == OSI_FUNCS_TIME_BLOCKING) {
        return (int32_t)xQueueReceive(queue, item, portMAX_DELAY);
    } else {
        return (int32_t)xQueueReceive(queue, item, block_time_tick);
    }
}

static uint32_t event_group_wait_bits_wrapper(void *event, uint32_t bits_to_wait_for, int clear_on_exit, int wait_for_all_bits, uint32_t block_time_tick)
{
    if (block_time_tick == OSI_FUNCS_TIME_BLOCKING) {
        return (uint32_t)xEventGroupWaitBits(event, bits_to_wait_for, clear_on_exit, wait_for_all_bits, portMAX_DELAY);
    } else {
        return (uint32_t)xEventGroupWaitBits(event, bits_to_wait_for, clear_on_exit, wait_for_all_bits, block_time_tick);
    }
}

static int32_t task_create_pinned_to_core_wrapper(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle, uint32_t core_id)
{
    return (uint32_t)xTaskCreatePinnedToCore(task_func, name, stack_depth, param, prio, task_handle, (core_id < portNUM_PROCESSORS ? core_id : tskNO_AFFINITY));
}

static int32_t task_create_wrapper(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle)
{
    return (uint32_t)xTaskCreate(task_func, name, stack_depth, param, prio, task_handle);
}

static int32_t IRAM_ATTR task_ms_to_tick_wrapper(uint32_t ms)
{
    return (int32_t)(ms / portTICK_PERIOD_MS);
}

static int32_t task_get_max_priority_wrapper(void)
{
    return (int32_t)(configMAX_PRIORITIES);
}

static int32_t esp_event_post_wrapper(const char* event_base, int32_t event_id, void* event_data, size_t event_data_size, uint32_t ticks_to_wait)
{
    if (ticks_to_wait == OSI_FUNCS_TIME_BLOCKING) {
        return (int32_t)esp_event_post(event_base, event_id, event_data, event_data_size, portMAX_DELAY);
    } else {
        return (int32_t)esp_event_post(event_base, event_id, event_data, event_data_size, ticks_to_wait);
    }
}

static void IRAM_ATTR wifi_apb80m_request_wrapper(void)
{
#ifdef CONFIG_PM_ENABLE
    wifi_apb80m_request();
#endif
}

static void IRAM_ATTR wifi_apb80m_release_wrapper(void)
{
#ifdef CONFIG_PM_ENABLE
    wifi_apb80m_release();
#endif
}

static void IRAM_ATTR timer_arm_wrapper(void *timer, uint32_t tmout, bool repeat)
{
    ets_timer_arm(timer, tmout, repeat);
}

static void IRAM_ATTR timer_disarm_wrapper(void *timer)
{
    ets_timer_disarm(timer);
}

static void timer_done_wrapper(void *ptimer)
{
    ets_timer_done(ptimer);
}

static void timer_setfn_wrapper(void *ptimer, void *pfunction, void *parg)
{
    ets_timer_setfn(ptimer, pfunction, parg);
}

static void IRAM_ATTR timer_arm_us_wrapper(void *ptimer, uint32_t us, bool repeat)
{
    ets_timer_arm_us(ptimer, us, repeat);
}

static void wifi_reset_mac_wrapper(void)
{
    periph_module_reset(PERIPH_WIFI_MODULE);
}

static void IRAM_ATTR wifi_rtc_enable_iso_wrapper(void)
{
#if CONFIG_MAC_BB_PD
    esp_mac_bb_power_down();
#endif
}

static void IRAM_ATTR wifi_rtc_disable_iso_wrapper(void)
{
#if CONFIG_MAC_BB_PD
    esp_mac_bb_power_up();
#endif
}

static void wifi_clock_enable_wrapper(void)
{
    wifi_module_enable();
}

static void wifi_clock_disable_wrapper(void)
{
    wifi_module_disable();
}

static int get_time_wrapper(void *t)
{
    return os_get_time(t);
}

static uint32_t esp_clk_slowclk_cal_get_wrapper(void)
{
    /* The bit width of WiFi light sleep clock calibration is 12 while the one of
     * system is 19. It should shift 19 - 12 = 7.
    */
    if (GET_PERI_REG_MASK(SYSTEM_BT_LPCK_DIV_FRAC_REG, SYSTEM_LPCLK_SEL_XTAL)) {
        uint64_t time_per_us = 1000000ULL;
        return (((time_per_us << RTC_CLK_CAL_FRACT) / (MHZ)) >> (RTC_CLK_CAL_FRACT - SOC_WIFI_LIGHT_SLEEP_CLK_WIDTH));
    } else {
        return (esp_clk_slowclk_cal_get() >> (RTC_CLK_CAL_FRACT - SOC_WIFI_LIGHT_SLEEP_CLK_WIDTH));
    }
}

static void * IRAM_ATTR malloc_internal_wrapper(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_8BIT|MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
}

static void * IRAM_ATTR realloc_internal_wrapper(void *ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT|MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
}

static void * IRAM_ATTR calloc_internal_wrapper(size_t n, size_t size)
{
    return heap_caps_calloc(n, size, MALLOC_CAP_8BIT|MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
}

static void * IRAM_ATTR zalloc_internal_wrapper(size_t size)
{
    void *ptr = heap_caps_calloc(1, size, MALLOC_CAP_8BIT|MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    return ptr;
}

static esp_err_t nvs_open_wrapper(const char* name, uint32_t open_mode, nvs_handle_t *out_handle)
{
    return nvs_open(name,(nvs_open_mode_t)open_mode, out_handle);
}

static void esp_log_writev_wrapper(uint32_t level, const char *tag, const char *format, va_list args)
{
    return esp_log_writev((esp_log_level_t)level,tag,format,args);
}

static void esp_log_write_wrapper(uint32_t level,const char *tag,const char *format, ...)
{
    va_list list;
    va_start(list, format);
    esp_log_writev((esp_log_level_t)level, tag, format, list);
    va_end(list);
}

static esp_err_t esp_read_mac_wrapper(uint8_t* mac, uint32_t type)
{
    return esp_read_mac(mac, (esp_mac_type_t)type);
}

static int coex_init_wrapper(void)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_init();
#else
    return 0;
#endif
}

static void coex_deinit_wrapper(void)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    coex_deinit();
#endif
}

static int coex_enable_wrapper(void)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_enable();
#else
    return 0;
#endif
}

static void coex_disable_wrapper(void)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    coex_disable();
#endif
}

static IRAM_ATTR uint32_t coex_status_get_wrapper(void)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_status_get();
#else
    return 0;
#endif
}

static void coex_condition_set_wrapper(uint32_t type, bool dissatisfy)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    coex_condition_set(type, dissatisfy);
#endif
}

static int coex_wifi_request_wrapper(uint32_t event, uint32_t latency, uint32_t duration)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_wifi_request(event, latency, duration);
#else
    return 0;
#endif
}

static IRAM_ATTR int coex_wifi_release_wrapper(uint32_t event)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_wifi_release(event);
#else
    return 0;
#endif
}

static int coex_wifi_channel_set_wrapper(uint8_t primary, uint8_t secondary)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_wifi_channel_set(primary, secondary);
#else
    return 0;
#endif
}

static IRAM_ATTR int coex_event_duration_get_wrapper(uint32_t event, uint32_t *duration)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_event_duration_get(event, duration);
#else
    return 0;
#endif
}

static int coex_pti_get_wrapper(uint32_t event, uint8_t *pti)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_pti_get(event, pti);
#else
    return 0;
#endif
}

static void coex_schm_status_bit_clear_wrapper(uint32_t type, uint32_t status)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    coex_schm_status_bit_clear(type, status);
#endif
}

static void coex_schm_status_bit_set_wrapper(uint32_t type, uint32_t status)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    coex_schm_status_bit_set(type, status);
#endif
}

static IRAM_ATTR int coex_schm_interval_set_wrapper(uint32_t interval)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_schm_interval_set(interval);
#else
    return 0;
#endif
}

static uint32_t coex_schm_interval_get_wrapper(void)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_schm_interval_get();
#else
    return 0;
#endif
}

static uint8_t coex_schm_curr_period_get_wrapper(void)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_schm_curr_period_get();
#else
    return 0;
#endif
}

static void * coex_schm_curr_phase_get_wrapper(void)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_schm_curr_phase_get();
#else
    return NULL;
#endif
}

static int coex_schm_curr_phase_idx_set_wrapper(int idx)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_schm_curr_phase_idx_set(idx);
#else
    return 0;
#endif
}

static int coex_schm_curr_phase_idx_get_wrapper(void)
{
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE
    return coex_schm_curr_phase_idx_get();
#else
    return 0;
#endif
}

static void IRAM_ATTR esp_empty_wrapper(void)
{

}

static void esp_phy_enable_wrapper(void)
{
    esp_phy_enable();
    phy_wifi_enable_set(1);
}

static void esp_phy_disable_wrapper(void)
{
    phy_wifi_enable_set(0);
    esp_phy_disable();
}

wifi_osi_funcs_t g_wifi_osi_funcs = {
    ._version = ESP_WIFI_OS_ADAPTER_VERSION,
    ._env_is_chip = env_is_chip_wrapper,
    ._set_intr = set_intr_wrapper,
    ._clear_intr = clear_intr_wrapper,
    ._set_isr = set_isr_wrapper,
    ._ints_on = enable_intr_wrapper,
    ._ints_off = disable_intr_wrapper,
    ._is_from_isr = is_from_isr_wrapper,
    ._spin_lock_create = spin_lock_create_wrapper,
    ._spin_lock_delete = free,
    ._wifi_int_disable = wifi_int_disable_wrapper,
    ._wifi_int_restore = wifi_int_restore_wrapper,
    ._task_yield_from_isr = task_yield_from_isr_wrapper,
    ._semphr_create = semphr_create_wrapper,
    ._semphr_delete = semphr_delete_wrapper,
    ._semphr_take = semphr_take_wrapper,
    ._semphr_give = semphr_give_wrapper,
    ._wifi_thread_semphr_get = wifi_thread_semphr_get_wrapper,
    ._mutex_create = mutex_create_wrapper,
    ._recursive_mutex_create = recursive_mutex_create_wrapper,
    ._mutex_delete = mutex_delete_wrapper,
    ._mutex_lock = mutex_lock_wrapper,
    ._mutex_unlock = mutex_unlock_wrapper,
    ._queue_create = queue_create_wrapper,
    ._queue_delete = (void(*)(void *))vQueueDelete,
    ._queue_send = queue_send_wrapper,
    ._queue_send_from_isr = queue_send_from_isr_wrapper,
    ._queue_send_to_back = queue_send_to_back_wrapper,
    ._queue_send_to_front = queue_send_to_front_wrapper,
    ._queue_recv = queue_recv_wrapper,
    ._queue_msg_waiting = (uint32_t(*)(void *))uxQueueMessagesWaiting,
    ._event_group_create = (void *(*)(void))xEventGroupCreate,
    ._event_group_delete = (void(*)(void *))vEventGroupDelete,
    ._event_group_set_bits = (uint32_t(*)(void *,uint32_t))xEventGroupSetBits,
    ._event_group_clear_bits = (uint32_t(*)(void *,uint32_t))xEventGroupClearBits,
    ._event_group_wait_bits = event_group_wait_bits_wrapper,
    ._task_create_pinned_to_core = task_create_pinned_to_core_wrapper,
    ._task_create = task_create_wrapper,
    ._task_delete = (void(*)(void *))vTaskDelete,
    ._task_delay = vTaskDelay,
    ._task_ms_to_tick = task_ms_to_tick_wrapper,
    ._task_get_current_task = (void *(*)(void))xTaskGetCurrentTaskHandle,
    ._task_get_max_priority = task_get_max_priority_wrapper,
    ._malloc = malloc,
    ._free = free,
    ._event_post = esp_event_post_wrapper,
    ._get_free_heap_size = esp_get_free_internal_heap_size,
    ._rand = esp_random,
    ._dport_access_stall_other_cpu_start_wrap = esp_empty_wrapper,
    ._dport_access_stall_other_cpu_end_wrap = esp_empty_wrapper,
    ._wifi_apb80m_request = wifi_apb80m_request_wrapper,
    ._wifi_apb80m_release = wifi_apb80m_release_wrapper,
    ._phy_disable = esp_phy_disable_wrapper,
    ._phy_enable = esp_phy_enable_wrapper,
    ._phy_update_country_info = esp_phy_update_country_info,
    ._read_mac = esp_read_mac_wrapper,
    ._timer_arm = timer_arm_wrapper,
    ._timer_disarm = timer_disarm_wrapper,
    ._timer_done = timer_done_wrapper,
    ._timer_setfn = timer_setfn_wrapper,
    ._timer_arm_us = timer_arm_us_wrapper,
    ._wifi_reset_mac = wifi_reset_mac_wrapper,
    ._wifi_clock_enable = wifi_clock_enable_wrapper,
    ._wifi_clock_disable = wifi_clock_disable_wrapper,
    ._wifi_rtc_enable_iso = wifi_rtc_enable_iso_wrapper,
    ._wifi_rtc_disable_iso = wifi_rtc_disable_iso_wrapper,
    ._esp_timer_get_time = esp_timer_get_time,
    ._nvs_set_i8 = nvs_set_i8,
    ._nvs_get_i8 = nvs_get_i8,
    ._nvs_set_u8 = nvs_set_u8,
    ._nvs_get_u8 = nvs_get_u8,
    ._nvs_set_u16 = nvs_set_u16,
    ._nvs_get_u16 = nvs_get_u16,
    ._nvs_open = nvs_open_wrapper,
    ._nvs_close = nvs_close,
    ._nvs_commit = nvs_commit,
    ._nvs_set_blob = nvs_set_blob,
    ._nvs_get_blob = nvs_get_blob,
    ._nvs_erase_key = nvs_erase_key,
    ._get_random = os_get_random,
    ._get_time = get_time_wrapper,
    ._random = os_random,
    ._slowclk_cal_get = esp_clk_slowclk_cal_get_wrapper,
    ._log_write = esp_log_write_wrapper,
    ._log_writev = esp_log_writev_wrapper,
    ._log_timestamp = esp_log_timestamp,
    ._malloc_internal =  malloc_internal_wrapper,
    ._realloc_internal = realloc_internal_wrapper,
    ._calloc_internal = calloc_internal_wrapper,
    ._zalloc_internal = zalloc_internal_wrapper,
    ._wifi_malloc = wifi_malloc,
    ._wifi_realloc = wifi_realloc,
    ._wifi_calloc = wifi_calloc,
    ._wifi_zalloc = wifi_zalloc_wrapper,
    ._wifi_create_queue = wifi_create_queue_wrapper,
    ._wifi_delete_queue = wifi_delete_queue_wrapper,
    ._coex_init = coex_init_wrapper,
    ._coex_deinit = coex_deinit_wrapper,
    ._coex_enable = coex_enable_wrapper,
    ._coex_disable = coex_disable_wrapper,
    ._coex_status_get = coex_status_get_wrapper,
    ._coex_condition_set = coex_condition_set_wrapper,
    ._coex_wifi_request = coex_wifi_request_wrapper,
    ._coex_wifi_release = coex_wifi_release_wrapper,
    ._coex_wifi_channel_set = coex_wifi_channel_set_wrapper,
    ._coex_event_duration_get = coex_event_duration_get_wrapper,
    ._coex_pti_get = coex_pti_get_wrapper,
    ._coex_schm_status_bit_clear = coex_schm_status_bit_clear_wrapper,
    ._coex_schm_status_bit_set = coex_schm_status_bit_set_wrapper,
    ._coex_schm_interval_set = coex_schm_interval_set_wrapper,
    ._coex_schm_interval_get = coex_schm_interval_get_wrapper,
    ._coex_schm_curr_period_get = coex_schm_curr_period_get_wrapper,
    ._coex_schm_curr_phase_get = coex_schm_curr_phase_get_wrapper,
    ._coex_schm_curr_phase_idx_set = coex_schm_curr_phase_idx_set_wrapper,
    ._coex_schm_curr_phase_idx_get = coex_schm_curr_phase_idx_get_wrapper,
    ._magic = ESP_WIFI_OS_ADAPTER_MAGIC,
};

coex_adapter_funcs_t g_coex_adapter_funcs = {
    ._version = COEX_ADAPTER_VERSION,
    ._task_yield_from_isr = task_yield_from_isr_wrapper,
    ._semphr_create = semphr_create_wrapper,
    ._semphr_delete = semphr_delete_wrapper,
    ._semphr_take_from_isr = semphr_take_from_isr_wrapper,
    ._semphr_give_from_isr = semphr_give_from_isr_wrapper,
    ._semphr_take = semphr_take_wrapper,
    ._semphr_give = semphr_give_wrapper,
    ._is_in_isr = xPortInIsrContext,
    ._malloc_internal =  malloc_internal_wrapper,
    ._free = free,
    ._esp_timer_get_time = esp_timer_get_time,
    ._magic = COEX_ADAPTER_MAGIC,
};
