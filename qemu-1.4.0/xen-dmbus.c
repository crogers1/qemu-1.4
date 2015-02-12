#include <libv4v.h>
#include <libdmbus.h>

#include "xen-dmbus.h"
#include "hw/xen.h"
#include "qemu/timer.h"

struct service {
    int fd;
    v4v_addr_t peer;
    const struct dmbus_ops *ops;
    void *opaque;
    struct dmbus_conn_prologue prologue;

    char buff[DMBUS_MAX_MSG_LEN];
    int len;

    QEMUTimer *reconnect_timer;
};

static void handle_message(struct service *s, union dmbus_msg *m)
{
    if (!s->ops) {
        return;
    }

    switch (m->hdr.msg_type) {
    case DMBUS_MSG_DOM0_INPUT_EVENT:
    {
        struct msg_dom0_input_event *msg = &m->dom0_input_event;

        if (s->ops->dom0_input_event) {
            s->ops->dom0_input_event(s->opaque, msg->type,
                                     msg->code, msg->value);
        }
        break;
    }
    case DMBUS_MSG_DISPLAY_INFO:
    {
        struct msg_display_info *msg = &m->display_info;

        if (s->ops->display_info) {
            s->ops->display_info(s->opaque, msg->DisplayID,
                                            msg->max_xres,
                                            msg->max_yres,
                                            msg->align);
        }
        break;
    }
    case DMBUS_MSG_DISPLAY_EDID:
    {
        struct msg_display_edid *msg = &m->display_edid;

        if (s->ops->display_edid) {
            s->ops->display_edid(s->opaque, msg->DisplayID,
                                            msg->edid.b);
        }
        break;
    }
    case DMBUS_MSG_DEVICE_MODEL_READY:
    {
         /* This space in intentionally left blank. */
         break;
    }
    case DMBUS_MSG_INPUT_CONFIG:
    {
        struct msg_input_config *msg = &m->input_config;

        if (s->ops->input_config) {
            s->ops->input_config(s->opaque, &msg->c);
        }
        break;
    }
    case DMBUS_MSG_INPUT_CONFIG_RESET:
    {
        struct msg_input_config_reset *msg = &m->input_config_reset;

        if (s->ops->input_config_reset) {
            s->ops->input_config_reset(s->opaque, msg->slot);
        }
        break;
    }
    default:
        fprintf(stderr, "%s: Unrecognized message ID: %d\n",
                __func__, m->hdr.msg_type);
    }
}

static void pop_message(struct service *s)
{

    union dmbus_msg *m = (union dmbus_msg *)s->buff;
    int len = m->hdr.msg_len;

    if ((s->len < sizeof(struct dmbus_msg_hdr)) ||
        (s->len < len)) {
        return;
    }

    memmove(s->buff,  s->buff + len,  s->len - len);
    s->len -= len;
}

static void handle_disconnect(struct service *s)
{
    if (qemu_timer_pending(s->reconnect_timer)) {
        return;
    }

    qemu_set_fd_handler(s->fd, NULL, NULL, NULL);
    v4v_close(s->fd);
    fprintf(stderr, "Remote service disconnected, scheduling reconnection.\n");
    qemu_mod_timer(s->reconnect_timer, qemu_get_clock_ms(rt_clock) + 1000);
}

static union dmbus_msg *sync_recv(struct service *s)
{
    int rc;
    union dmbus_msg *m = (union dmbus_msg *)s->buff;

    while ((s->len < sizeof(struct dmbus_msg_hdr)) ||
           (s->len < m->hdr.msg_len)) {

        rc = v4v_recv(s->fd, s->buff + s->len, sizeof(s->buff) - s->len, 0);
        switch (rc) {
        case 0:
            handle_disconnect(s);
            return NULL;
        case -1:
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "%s: recv error: %s\n",
                    __func__, strerror(errno));
            return NULL;
        default:
            s->len += rc;
        }

    }

    return m;
}

static void dmbus_fd_handler(void *opaque)
{
    int rc;
    struct service *s = opaque;
    union dmbus_msg *m = (union dmbus_msg *)s->buff;

    do {
        rc = v4v_recv(s->fd, s->buff + s->len, sizeof(s->buff) - s->len,
                      MSG_DONTWAIT);

        switch (rc) {
        case 0:
            handle_disconnect(s);
            return;
        case -1:
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "%s: recv error: %s\n",
                    __func__, strerror(errno));
            return;
        default:
            s->len += rc;
        }
    } while (rc <= 0);
    m = sync_recv(s);
    if (!m) {
        return;
    }

    while ((s->len >= sizeof(struct dmbus_msg_hdr)) &&
           (s->len >= m->hdr.msg_len)) {

        handle_message(s, m);
        pop_message(s);
    }
}

int dmbus_sync_recv(dmbus_service_t service, int type,
                    void *data, size_t size)
{
    struct service *s = service;
    union dmbus_msg *m;

    m = sync_recv(s);
    if (!m) {
        return -1;
    }

    while (m->hdr.msg_type != type) {
        handle_message(s, m);
        pop_message(s);
        m = sync_recv(s);
        if (!m) {
            return -1;
        }
    }

    if (size > m->hdr.msg_len) {
        size = m->hdr.msg_len;
    }

    memcpy(data, m, size);
    pop_message(s);

    return size;
}

static void try_reconnect(void *opaque)
{
    struct service *s = opaque;
    int rc;

    s->fd = v4v_socket(SOCK_STREAM);
    if (s->fd == -1) {
        goto rearm;
    }
    rc = v4v_connect(s->fd, &s->peer);
    if (rc == -1) {
        v4v_close(s->fd);
        goto rearm;
    }
    rc = v4v_send(s->fd, &s->prologue, sizeof(s->prologue), 0);
    if (rc != sizeof(s->prologue)) {
        v4v_close(s->fd);
        goto rearm;
    }

    if (s->ops->reconnect) {
        s->ops->reconnect(s->opaque);
    }

    qemu_set_fd_handler(s->fd, dmbus_fd_handler, NULL, s);

    return;
rearm:
    qemu_mod_timer(s->reconnect_timer, qemu_get_clock_ms(rt_clock) + 1000);
}

static void fill_hash(uint8_t *h)
{
    const char *hash_str = DMBUS_SHA1_STRING;
    size_t i;

    for (i = 0; i < 20; i++) {
        unsigned int c;

        sscanf(hash_str + 2 * i, "%02x", &c);
        h[i] = c;
    }
}

dmbus_service_t
dmbus_service_connect(int service,
                      DeviceType devtype,
                      const struct dmbus_ops *ops,
                      void *opaque)
{
    struct service *s;
    int rc;

    s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }

    s->fd = v4v_socket(SOCK_STREAM);
    if (s->fd == -1) {
        fprintf(stderr, "%s: Failed to create v4v socket: %s\n",
                __func__, strerror(errno));
        free(s);
        return NULL;
    }

    s->peer.port = DMBUS_BASE_PORT + service;
    s->peer.domain = 0; /* Dom0 */

    rc = v4v_connect(s->fd, &s->peer);
    if (rc == -1) {
        fprintf(stderr, "%s: Failed to connect v4v socket: %s\n",
                __func__, strerror(errno));
        goto close;
    }

    s->prologue.domain = xen_domid;
    s->prologue.type = devtype;
    fill_hash(s->prologue.hash);

    rc = v4v_send(s->fd, &s->prologue, sizeof(s->prologue), 0);
    if (rc != sizeof(s->prologue)) {
        fprintf(stderr, "%s: Failed to initialize dmbus connection: %s\n",
                __func__, strerror(errno));
        goto close;
    }

    s->opaque = opaque;
    s->ops = ops;
    s->reconnect_timer = qemu_new_timer_ms(rt_clock, try_reconnect, s);

    qemu_set_fd_handler(s->fd, dmbus_fd_handler, NULL, s);

    return s;
close:
    v4v_close(s->fd);
    free(s);
    return NULL;
}

void
dmbus_service_disconnect(dmbus_service_t service)
{
    struct service *s = service;

    qemu_set_fd_handler(s->fd, NULL, NULL, NULL);
    qemu_free_timer(s->reconnect_timer);
    v4v_close(s->fd);
    free(s);
}

int
dmbus_send(dmbus_service_t service,
           int msgtype,
           void *data,
           size_t len)
{
    struct service *s = service;
    struct dmbus_msg_hdr *hdr = data;
    int rc;
    size_t b = 0;

    hdr->msg_type = msgtype;
    hdr->msg_len = len;

    while (b < len) {
        rc = v4v_send(s->fd, data + b, len - b, 0);
        if (rc == -1) {
            if (errno == ECONNRESET) {
                handle_disconnect(s);
            } else {
                fprintf(stderr, "%s failed: %s\n",
                        __func__, strerror(errno));
            }
            return -1;
        }

        b += rc;
    }

    return b;
}
