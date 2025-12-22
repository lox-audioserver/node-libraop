#include <arpa/inet.h>
#include <stdbool.h>
#include <stdlib.h>
#include "mdnssvc.h"
#include "mdnssd.h"

struct mdnsd {
  struct in_addr host;
};

struct mdns_service {
  struct mdns_service *next;
};

struct mdnssd_handle_s {};

struct mdnsd *mdnsd_start(struct in_addr host, bool verbose) {
  (void)verbose;
  struct mdnsd *ctx = (struct mdnsd *)calloc(1, sizeof(struct mdnsd));
  if (ctx) ctx->host = host;
  return ctx;
}

void mdnsd_stop(struct mdnsd *s) {
  free(s);
}

void mdnsd_set_hostname(struct mdnsd *svr, const char *hostname, struct in_addr addr) {
  (void)svr;
  (void)hostname;
  (void)addr;
}

struct mdns_service *mdnsd_register_svc(struct mdnsd *svr, const char *instance_name,
                                        const char *type, uint16_t port,
                                        const char *hostname, const char *txt[]) {
  (void)svr;
  (void)instance_name;
  (void)type;
  (void)port;
  (void)hostname;
  (void)txt;
  return (struct mdns_service *)calloc(1, sizeof(struct mdns_service));
}

void mdns_service_destroy(struct mdns_service *srv) {
  free(srv);
}

void mdns_service_remove(struct mdnsd *svr, struct mdns_service *svc) {
  (void)svr;
  mdns_service_destroy(svc);
}

struct mdnssd_handle_s *mdnssd_init(int dbg, struct in_addr host, bool compliant) {
  (void)dbg;
  (void)host;
  (void)compliant;
  return (struct mdnssd_handle_s *)calloc(1, sizeof(struct mdnssd_handle_s));
}

bool mdnssd_query(struct mdnssd_handle_s *handle, const char *query_arg, bool unicast,
                  int runtime, mdns_callback_t *callback, void *cookie) {
  (void)handle;
  (void)query_arg;
  (void)unicast;
  (void)runtime;
  (void)callback;
  (void)cookie;
  return false;
}

void mdnssd_control(struct mdnssd_handle_s *handle, mdnssd_control_e request) {
  (void)handle;
  (void)request;
}

void mdnssd_close(struct mdnssd_handle_s *handle) {
  free(handle);
}

void mdnssd_free_list(mdnssd_service_t *slist) {
  mdnssd_service_t *s = slist;
  while (s) {
    mdnssd_service_t *n = s->next;
    free(s->name);
    free(s->hostname);
    free(s->attr);
    free(s);
    s = n;
  }
}

mdnssd_service_t *mdnssd_get_list(struct mdnssd_handle_s *handle) {
  (void)handle;
  return NULL;
}
