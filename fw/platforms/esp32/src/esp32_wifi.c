/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#include <stdbool.h>
#include <string.h>

#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "apps/dhcpserver.h"

#include "fw/src/miot_sys_config.h"
#include "fw/src/miot_wifi.h"

esp_err_t wifi_event_handler(system_event_t *event) {
  int mg_ev = -1;
  bool pass_to_system = true;
  switch (event->event_id) {
    case SYSTEM_EVENT_STA_DISCONNECTED:
      mg_ev = MIOT_WIFI_DISCONNECTED;
      break;
    case SYSTEM_EVENT_STA_CONNECTED:
      mg_ev = MIOT_WIFI_CONNECTED;
      break;
    case SYSTEM_EVENT_STA_GOT_IP:
      /*
       * This event is forwarded to us from system handler, don't pass it on.
       * https://github.com/espressif/esp-idf/issues/161
       */
      mg_ev = MIOT_WIFI_IP_ACQUIRED;
      pass_to_system = false;
      break;
    case SYSTEM_EVENT_AP_STACONNECTED: {
      const uint8_t *mac = event->event_info.sta_connected.mac;
      LOG(LL_INFO, ("WiFi AP: station %02X%02X%02X%02X%02X%02X (aid %d) %s",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                    event->event_info.sta_connected.aid, "connected"));
      break;
    }
    case SYSTEM_EVENT_AP_STADISCONNECTED: {
      const uint8_t *mac = event->event_info.sta_disconnected.mac;
      LOG(LL_INFO, ("WiFi AP: station %02X%02X%02X%02X%02X%02X (aid %d) %s",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                    event->event_info.sta_disconnected.aid, "disconnected"));
      break;
    }
    default:
      LOG(LL_INFO, ("WiFi event: %d", event->event_id));
  }

  /* TODO(rojer): Post via miot task */
  if (mg_ev >= 0) miot_wifi_on_change_cb(mg_ev);

  return (pass_to_system ? esp_event_send(event) : ESP_OK);
}

static esp_err_t miot_wifi_set_mode(wifi_mode_t mode) {
  LOG(LL_INFO,
      ("WiFi mode: %s",
       (mode == WIFI_MODE_AP ? "AP" : mode == WIFI_MODE_STA
                                          ? "STA"
                                          : mode == WIFI_MODE_APSTA ? "AP+STA"
                                                                    : "???")));
  esp_err_t r = esp_wifi_set_mode(mode);
  if (r == ESP_ERR_WIFI_NOT_INIT) {
    wifi_init_config_t icfg = {.event_handler = wifi_event_handler};
    r = esp_wifi_init(&icfg);
    if (r != ESP_OK) {
      LOG(LL_ERROR, ("Failed to init WiFi: %d", r));
      return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    r = esp_wifi_set_mode(mode);
  }

  if (r != ESP_OK) {
    LOG(LL_ERROR, ("Failed to set WiFi mode %d: %d", mode, r));
    return r;
  }

  return ESP_OK;
}

static esp_err_t miot_wifi_add_mode(wifi_mode_t mode) {
  esp_err_t r;
  wifi_mode_t cur_mode = WIFI_MODE_NULL;
  r = esp_wifi_get_mode(&cur_mode);
  /* If WIFI is not initialized yet, set_mode will do it. */
  if (r != ESP_OK && r != ESP_ERR_WIFI_NOT_INIT) {
    return r;
  }

  if (cur_mode == mode || cur_mode == WIFI_MODE_APSTA) {
    return ESP_OK;
  }

  if ((cur_mode == WIFI_MODE_AP && mode == WIFI_MODE_STA) ||
      (cur_mode == WIFI_MODE_STA && mode == WIFI_MODE_AP)) {
    mode = WIFI_MODE_APSTA;
  }

  return miot_wifi_set_mode(mode);
}

static esp_err_t miot_wifi_remove_mode(wifi_mode_t mode) {
  esp_err_t r;
  wifi_mode_t cur_mode;
  r = esp_wifi_get_mode(&cur_mode);
  if (r == ESP_ERR_WIFI_NOT_INIT) {
    /* Not initialized at all? Ok then. */
    return ESP_OK;
  }
  if ((mode == WIFI_MODE_STA && cur_mode == WIFI_MODE_AP) ||
      (mode == WIFI_MODE_AP && cur_mode == WIFI_MODE_STA)) {
    /* Nothing to do. */
    return ESP_OK;
  }
  if (mode == WIFI_MODE_APSTA ||
      (mode == WIFI_MODE_STA && cur_mode == WIFI_MODE_STA) ||
      (mode == WIFI_MODE_AP && cur_mode == WIFI_MODE_AP)) {
    LOG(LL_INFO, ("WiFi disabled"));
    return esp_wifi_stop();
  }
  /* As a result we will always remain in STA-only or AP-only mode. */
  return miot_wifi_set_mode(mode == WIFI_MODE_STA ? WIFI_MODE_AP
                                                  : WIFI_MODE_STA);
}

int miot_wifi_setup_sta(const struct sys_config_wifi_sta *cfg) {
  esp_err_t r;
  wifi_config_t wcfg;
  memset(&wcfg, 0, sizeof(wcfg));
  wifi_sta_config_t *stacfg = &wcfg.sta;

  if (!cfg->enable) {
    return (miot_wifi_remove_mode(WIFI_MODE_STA) == ESP_OK);
  }

  r = miot_wifi_add_mode(WIFI_MODE_STA);
  if (r != ESP_OK) return false;

  strncpy(stacfg->ssid, cfg->ssid, sizeof(stacfg->ssid));
  if (cfg->pass != NULL) {
    strncpy(stacfg->password, cfg->pass, sizeof(stacfg->password));
  }

  if (cfg->ip != NULL && cfg->netmask != NULL) {
    tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
    tcpip_adapter_ip_info_t info;
    memset(&info, 0, sizeof(info));
    info.ip.addr = ipaddr_addr(cfg->ip);
    info.netmask.addr = ipaddr_addr(cfg->netmask);
    if (cfg->gw != NULL) info.gw.addr = ipaddr_addr(cfg->gw);
    r = tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &info);
    if (r != ESP_OK) {
      LOG(LL_ERROR, ("Failed to set WiFi STA IP config: %d", r));
      return false;
    }
    LOG(LL_INFO, ("WiFi STA IP config: %s %s %s", cfg->ip, cfg->netmask,
                  (cfg->gw ? cfg->gw : "")));
  } else {
    tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
  }

  r = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
  if (r != ESP_OK) {
    LOG(LL_ERROR, ("Failed to set STA config: %d", r));
    return false;
  }

  r = esp_wifi_connect();
  if (r == ESP_ERR_WIFI_NOT_START) {
    r = esp_wifi_start();
    if (r != ESP_OK) {
      LOG(LL_ERROR, ("Failed to start WiFi: %d", r));
      return false;
    }
    r = esp_wifi_connect();
  }

  if (r == ESP_OK) {
    LOG(LL_INFO, ("WiFi STA: Joining %s", cfg->ssid));
  } else {
    LOG(LL_INFO, ("WiFi STA connect failed: %d", r));
  }

  return (r == ESP_OK);
}

int miot_wifi_setup_ap(const struct sys_config_wifi_ap *cfg) {
  esp_err_t r;
  wifi_config_t wcfg;
  memset(&wcfg, 0, sizeof(wcfg));
  wifi_ap_config_t *apcfg = &wcfg.ap;

  if (!cfg->enable) {
    return (miot_wifi_remove_mode(WIFI_MODE_AP) == ESP_OK);
  }

  r = miot_wifi_add_mode(WIFI_MODE_AP);
  if (r != ESP_OK) return false;

  strncpy(apcfg->ssid, cfg->ssid, sizeof(apcfg->ssid));
  miot_expand_mac_address_placeholders(apcfg->ssid);
  if (cfg->pass != NULL) {
    strncpy(apcfg->password, cfg->pass, sizeof(apcfg->password));
    apcfg->authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    apcfg->authmode = WIFI_AUTH_OPEN;
  }
  apcfg->channel = cfg->channel;
  apcfg->ssid_hidden = (cfg->hidden != 0);
  apcfg->max_connection = cfg->max_connections;
  apcfg->beacon_interval = 100; /* ms */

  r = esp_wifi_set_config(WIFI_IF_AP, &wcfg);
  if (r != ESP_OK) {
    LOG(LL_ERROR, ("Failed to set AP config: %d", r));
    return false;
  }

  tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
  {
    tcpip_adapter_ip_info_t info;
    memset(&info, 0, sizeof(info));
    info.ip.addr = ipaddr_addr(cfg->ip);
    info.netmask.addr = ipaddr_addr(cfg->netmask);
    if (cfg->gw != NULL) info.gw.addr = ipaddr_addr(cfg->gw);
    r = tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &info);
    if (r != ESP_OK) {
      LOG(LL_ERROR, ("Failed to set WiFi AP IP config: %d", r));
      return false;
    }
  }
  {
    dhcps_lease_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.enable = true;
    opt.start_ip.addr = ipaddr_addr(cfg->dhcp_start);
    opt.end_ip.addr = ipaddr_addr(cfg->dhcp_end);
    r = tcpip_adapter_dhcps_option(TCPIP_ADAPTER_OP_SET, REQUESTED_IP_ADDRESS,
                                   &opt, sizeof(opt));
    if (r != ESP_OK) {
      LOG(LL_ERROR, ("Failed to set WiFi AP DHCP config: %d", r));
      return false;
    }
  }
  LOG(LL_INFO, ("WiFi AP IP config: %s/%s gw %s, DHCP range %s - %s", cfg->ip,
                cfg->netmask, (cfg->gw ? cfg->gw : "(none)"), cfg->dhcp_start,
                cfg->dhcp_end));
  tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);
  /* There is no way to tell if AP is running already. */
  esp_wifi_start();

  LOG(LL_INFO, ("WiFi AP: SSID %s, channel %d", apcfg->ssid, apcfg->channel));

  return true;
}

static char *miot_wifi_get_ip(tcpip_adapter_if_t if_no) {
  tcpip_adapter_ip_info_t info;
  char *ip;
  if ((tcpip_adapter_get_ip_info(if_no, &info) != ESP_OK) ||
      info.ip.addr == 0) {
    return NULL;
  }
  if (asprintf(&ip, IPSTR, IP2STR(&info.ip)) < 0) {
    return NULL;
  }
  return ip;
}

char *miot_wifi_get_ap_ip(void) {
  return miot_wifi_get_ip(TCPIP_ADAPTER_IF_AP);
}

char *miot_wifi_get_sta_ip(void) {
  return miot_wifi_get_ip(TCPIP_ADAPTER_IF_STA);
}

static enum miot_init_result do_wifi(const struct sys_config_wifi *cfg) {
  bool result = false;
  if (cfg->ap.enable && !cfg->sta.enable) {
    result = miot_wifi_setup_ap(&cfg->ap);
  } else if (cfg->ap.enable && cfg->sta.enable && cfg->ap.keep_enabled) {
    result = (miot_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK &&
              miot_wifi_setup_ap(&cfg->ap) && miot_wifi_setup_sta(&cfg->sta));
  } else if (cfg->sta.enable) {
    result = miot_wifi_setup_sta(&cfg->sta);
  } else {
    LOG(LL_INFO, ("WiFi is disabled"));
    result = true;
  }
  return (result ? MIOT_INIT_OK : MIOT_INIT_CONFIG_WIFI_INIT_FAILED);
}
enum miot_init_result miot_sys_config_init_platform(struct sys_config *cfg) {
  /* TODO: UART settings */

  return do_wifi(&cfg->wifi);
}

void miot_wifi_hal_init(void) {
}
