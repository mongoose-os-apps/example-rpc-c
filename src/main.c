/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>

#include "mgos_rpc.h"

#include "common/cs_dbg.h"
#include "common/json_utils.h"
#include "common/platform.h"
#include "frozen.h"
#include "mgos_app.h"
#include "mgos_gpio.h"
#include "mgos_net.h"
#include "mgos_sys_config.h"
#include "mgos_timers.h"

static void inc_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                        struct mg_rpc_frame_info *fi, struct mg_str args) {
  struct mbuf fb;
  struct json_out out = JSON_OUT_MBUF(&fb);

  mbuf_init(&fb, 20);

  int num = 0;
  if (json_scanf(args.p, args.len, ri->args_fmt, &num) == 1) {
    json_printf(&out, "{num: %d}", num + 1);
  } else {
    json_printf(&out, "{error: %Q}", "num is required");
  }

  mgos_gpio_toggle(mgos_sys_config_get_board_led1_pin());

  printf("%d + 1 = %d\n", num, num + 1);

  mg_rpc_send_responsef(ri, "%.*s", fb.len, fb.buf);
  ri = NULL;

  mbuf_free(&fb);

  (void) cb_arg;
  (void) fi;
}

int peer_num = 111;

static void rpc_resp(struct mg_rpc *c, void *cb_arg,
                     struct mg_rpc_frame_info *fi, struct mg_str result,
                     int error_code, struct mg_str error_msg) {
  if (error_code == 0 && result.len > 0) {
    LOG(LL_INFO, ("response: %.*s", (int) result.len, result.p));
    json_scanf(result.p, result.len, "{num: %d}", &peer_num);
  } else {
    LOG(LL_INFO,
        ("error: %d %.*s", error_code, (int) error_msg.len, error_msg.p));
  }

  (void) c;
  (void) cb_arg;
  (void) fi;
  (void) error_msg;
}

static void call_specific_peer(const char *peer) {
  struct mg_rpc *c = mgos_rpc_get_global();
  struct mg_rpc_call_opts opts;
  LOG(LL_INFO, ("Calling %s", peer));
  memset(&opts, 0, sizeof(opts));
  opts.dst = mg_mk_str(peer);
  mg_rpc_callf(c, mg_mk_str("Example.Increment"), rpc_resp, NULL, &opts,
               "{num: %d}", peer_num);
}

static void call_peer(void) {
  const char *peer = mgos_sys_config_get_c_rpc_peer();
  if (peer == NULL) {
    LOG(LL_INFO, ("Peer address not configured, set c_rpc.peer (e.g. "
                  "ws://192.168.1.234/rpc)"));
    return;
  }
  call_specific_peer(peer);
}

void net_changed(int ev, void *evd, void *arg) {
  if (ev != MGOS_NET_EV_IP_ACQUIRED) return;
  call_peer();
  (void) evd;
  (void) arg;
}

static void button_cb(int pin, void *arg) {
  LOG(LL_INFO, ("Click!"));
  call_peer();
  (void) pin;
  (void) arg;
}

static void call_peer_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                              struct mg_rpc_frame_info *fi,
                              struct mg_str args) {
  char *peer = NULL;
  if (json_scanf(args.p, args.len, ri->args_fmt, &peer) == 1) {
    call_specific_peer(peer);
    free(peer);
  } else {
    call_peer();
  }
  mg_rpc_send_responsef(ri, NULL);

  (void) cb_arg;
  (void) fi;
}

static void rpc_info_timer_cb(void *arg) {
  struct mg_rpc_channel_info *cici = NULL;
  int num_ci = 0;
  if (!mg_rpc_get_channel_info(mgos_rpc_get_global(), &cici, &num_ci)) return;
  if (num_ci > 0) {
    LOG(LL_INFO, ("RPC channels (%d):", num_ci));
    for (int i = 0; i < num_ci; i++) {
      const struct mg_rpc_channel_info *ci = &cici[i];
      LOG(LL_INFO, ("  %d: type: %.*s, info: %.*s, dst: %.*s, open? %d, "
                    "persistent? %d, bcast_enabled? %d",
                    i, (int) ci->type.len, (ci->type.p ? ci->type.p : ""),
                    (int) ci->info.len, (ci->info.p ? ci->info.p : ""),
                    (int) ci->dst.len, (ci->dst.p ? ci->dst.p : ""),
                    ci->is_open, ci->is_persistent, ci->is_broadcast_enabled));
    }
  }
  mg_rpc_channel_info_free_all(cici, num_ci);
  (void) arg;
}

enum mgos_app_init_result mgos_app_init(void) {
  struct mg_rpc *c = mgos_rpc_get_global();
  mg_rpc_add_handler(c, "Example.Increment", "{num: %d}", inc_handler, NULL);
  mg_rpc_add_handler(c, "Example.CallPeer", "{peer: %Q}", call_peer_handler,
                     NULL);
  mgos_event_add_group_handler(MGOS_EVENT_GRP_NET, net_changed, NULL);
  int led_pin = mgos_sys_config_get_board_led1_pin();
  mgos_gpio_set_mode(led_pin, MGOS_GPIO_MODE_OUTPUT);
  mgos_gpio_write(led_pin, !mgos_sys_config_get_board_led1_active_high());

  enum mgos_gpio_pull_type btn_pull;
  enum mgos_gpio_int_mode btn_int_edge;
  if (mgos_sys_config_get_board_btn1_pull_up()) {
    btn_pull = MGOS_GPIO_PULL_UP;
    btn_int_edge = MGOS_GPIO_INT_EDGE_NEG;
  } else {
    btn_pull = MGOS_GPIO_PULL_DOWN;
    btn_int_edge = MGOS_GPIO_INT_EDGE_POS;
  }
  mgos_gpio_set_button_handler(mgos_sys_config_get_board_btn1_pin(), btn_pull,
                               btn_int_edge, 20 /* debounce_ms */, button_cb,
                               NULL);
  mgos_set_timer(1000, MGOS_TIMER_REPEAT, rpc_info_timer_cb, NULL);
  return MGOS_APP_INIT_SUCCESS;
}
