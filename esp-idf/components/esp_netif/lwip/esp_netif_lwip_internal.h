// Copyright 2015-2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_netif_slip.h"
#include "lwip/netif.h"

#ifdef CONFIG_ESP_NETIF_TCPIP_LWIP

struct esp_netif_netstack_lwip_vanilla_config {
    err_t (*init_fn)(struct netif*);
    void (*input_fn)(void *netif, void *buffer, size_t len, void *eb);
};

struct esp_netif_netstack_lwip_ppp_config {
    void (*input_fn)(void *netif, void *buffer, size_t len, void *eb);
    esp_netif_ppp_config_t ppp_events;
};

struct esp_netif_netstack_lwip_slip_config {
    err_t (*init_fn)(struct netif*);
    void (*input_fn)(void *netif, void *buffer, size_t len, void *eb);
    esp_netif_slip_config_t slip_config;
};

// LWIP netif specific network stack configuration
struct esp_netif_netstack_config {
    union {
        struct esp_netif_netstack_lwip_vanilla_config lwip;
        struct esp_netif_netstack_lwip_ppp_config lwip_ppp;
    };
};

struct esp_netif_api_msg_s;

typedef int (*esp_netif_api_fn)(struct esp_netif_api_msg_s *msg);

typedef struct esp_netif_api_msg_s {
    int type;  /**< The first field MUST be int */
    int ret;
    esp_netif_api_fn api_fn;
    union {
        esp_netif_t *esp_netif;
        esp_netif_callback_fn user_fn;
    };
    void    *data;
} esp_netif_api_msg_t;


typedef struct esp_netif_dns_param_s {
    esp_netif_dns_type_t dns_type;
    esp_netif_dns_info_t *dns_info;
} esp_netif_dns_param_t;

typedef struct esp_netif_ip_lost_timer_s {
    bool timer_running;
} esp_netif_ip_lost_timer_t;

/**
 * @brief Check the netif if of a specific P2P type
 */
#if CONFIG_PPP_SUPPORT || CONFIG_LWIP_SLIP_SUPPORT
#define _IS_NETIF_POINT2POINT_TYPE(netif, type) (netif->related_data && netif->related_data->is_point2point && netif->related_data->netif_type == type)
#else
#define _IS_NETIF_POINT2POINT_TYPE(netif, type) false
#endif

/**
 * @brief Additional netif types when related data are needed
 */
enum netif_types {
    COMMON_LWIP_NETIF,
    PPP_LWIP_NETIF,
    SLIP_LWIP_NETIF
};

/**
 * @brief Related data to esp-netif (additional data for some special types of netif
 * (typically for point-point network types, such as PPP or SLIP)
 */
typedef struct netif_related_data {
    bool is_point2point;
    enum netif_types netif_type;
} netif_related_data_t;

/**
 * @brief Main esp-netif container with interface related information
 */
struct esp_netif_obj {
    // default interface addresses
    uint8_t mac[NETIF_MAX_HWADDR_LEN];
    esp_netif_ip_info_t* ip_info;
    esp_netif_ip_info_t* ip_info_old;

    // lwip netif related
    struct netif *lwip_netif;
    err_t (*lwip_init_fn)(struct netif*);
    void (*lwip_input_fn)(void *input_netif_handle, void *buffer, size_t len, void *eb);
    void * netif_handle;    // netif impl context (either vanilla lwip-netif or ppp_pcb)
    netif_related_data_t *related_data; // holds additional data for specific netifs

    // io driver related
    void* driver_handle;
    esp_err_t (*driver_transmit)(void *h, void *buffer, size_t len);
    esp_err_t (*driver_transmit_wrap)(void *h, void *buffer, size_t len, void *pbuf);
    void (*driver_free_rx_buffer)(void *h, void* buffer);

    // dhcp related
    esp_netif_dhcp_status_t dhcpc_status;
    esp_netif_dhcp_status_t dhcps_status;
    bool timer_running;

    // event translation
    ip_event_t get_ip_event;
    ip_event_t lost_ip_event;

    // misc flags, types, keys, priority
    esp_netif_flags_t flags;
    char * hostname;
    char * if_key;
    char * if_desc;
    int route_prio;

    // mldv6 timer
    bool mldv6_report_timer_started;
};

#endif /* CONFIG_ESP_NETIF_TCPIP_LWIP */
