#include <errno.h>
#include <assert.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include "core.h"
#include "ringbuf.h"
#include "channel.h"

__attribute__((noreturn,format(printf,1,2)))
static void
die_proto_error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    die(ECOMM,
        "protocol error: %s",
        xavprintf(fmt, args));
}

static bool
detect_msg(struct ringbuf* rb, struct msg* mhdr)
{
    size_t avail = ringbuf_size(rb);
    if (avail < sizeof (*mhdr))
        return false;

    ringbuf_copy_out(rb, mhdr, sizeof (*mhdr));
    return mhdr->size <= avail;
}

static void
adbx_sh_process_msg_channel_data(struct adbx_sh* sh,
                                 struct msg_channel_data* m)
{
    unsigned nrch = sh->nrch;
    struct channel* cmdch = sh->ch[FROM_PEER];

    if (m->channel <= NR_SPECIAL_CH || m->channel > nrch)
            die_proto_error("invalid channel %d", m->channel);

    struct channel* c = sh->ch[m->channel];
    if (c->dir == CHANNEL_FROM_FD)
        die_proto_error("wrong channel direction");

    if (c->fdh == NULL)
        return;         /* Channel already closed */

    size_t payloadsz = m->msg.size - sizeof (m);

    /* If we received more data than will fit in the receive
     * buffer, peer didn't respect window requirements.  */
    if (ringbuf_room(c->rb) < payloadsz)
        die_proto_error("window desync");

    struct iovec iov[2];
    ringbuf_iov(cmdch->rb, iov, payloadsz);
    channel_write(c, iov, 2);
    ringbuf_note_removed(cmdch->rb, payloadsz);
}

static void
adbx_sh_process_msg_channel_window(struct adbx_sh* sh,
                                   struct msg_channel_window* m)
{
    unsigned nrch = sh->nrch;

    if (m->channel <= NR_SPECIAL_CH || m->channel > nrch)
        die_proto_error("invalid channel %d", m->channel);

    struct channel* c = sh->ch[m->channel];
    if (c->dir == CHANNEL_FROM_FD)
        die_proto_error("wrong channel direction");

    if (c->fdh == NULL)
        return;         /* Channel already closed */

    if (SATADD(&c->window, c->window, m->window_delta)) {
        die_proto_error("window overflow!?");
    }
}

static void
adbx_sh_process_msg_channel_close(struct adbx_sh* sh,
                                  struct msg_channel_close* m)
{
    unsigned nrch = sh->nrch;

    if (m->channel <= NR_SPECIAL_CH || m->channel > nrch)
        return;                 /* Ignore invalid close */

    struct channel* c = sh->ch[m->channel];
    c->sent_eof = true; /* Peer already knows we're closed. */
    channel_close(c);
}

void
adbx_sh_process_msg(struct adbx_sh* sh, struct msg mhdr)
{
    struct channel* cmdch = sh->ch[FROM_PEER];

    if (mhdr.type == MSG_CHANNEL_DATA) {
        struct msg_channel_data m;
        if (mhdr.size < sizeof (m))
            die_proto_error("wrong msg size");

        ringbuf_copy_out(cmdch->rb, &m, sizeof (m));
        ringbuf_note_removed(cmdch->rb, sizeof (m));
        adbx_sh_process_msg_channel_data(sh, &m);
    } else if (mhdr.type == MSG_CHANNEL_WINDOW) {
        struct msg_channel_window m;
        if (mhdr.size != sizeof (m))
            die_proto_error("wrong msg size");

        ringbuf_copy_out(cmdch->rb, &m, sizeof (m));
        ringbuf_note_removed(cmdch->rb, sizeof (m));
        adbx_sh_process_msg_channel_window(sh, &m);
    } else if (mhdr.type == MSG_CHANNEL_CLOSE) {
        struct msg_channel_close m;
        if (mhdr.size != sizeof (m))
            die_proto_error("wrong msg size");

        ringbuf_copy_out(cmdch->rb, &m, sizeof (m));
        ringbuf_note_removed(cmdch->rb, sizeof (m));
        adbx_sh_process_msg_channel_close(sh, &m);
    } else {
        ringbuf_note_removed(cmdch->rb, mhdr.size);
        die(ECOMM, "unrecognized command %d", mhdr.type);
    }
}

static size_t
adbx_maxoutmsg(struct adbx_sh* sh)
{
    return XMIN(sh->max_outgoing_msg,
                ringbuf_room(sh->ch[TO_PEER]->rb));
}

static void
xmit_acks(struct channel* c, unsigned chno, struct adbx_sh* sh)
{
    size_t maxoutmsg = adbx_maxoutmsg(sh);
    struct msg_channel_window m;

    if (c->bytes_written > 0 && maxoutmsg >= sizeof (m)) {
        memset(&m, 0, sizeof (m));
        m.msg.type = MSG_CHANNEL_WINDOW;
        m.msg.size = sizeof (m);
        m.channel = chno;
        m.window_delta = c->bytes_written;
        channel_write(sh->ch[TO_PEER], &(struct iovec){&m, sizeof (m)}, 1);
        c->bytes_written = 0;
    }
}

static void
xmit_data(struct channel* c,
          unsigned chno,
          struct adbx_sh* sh)
{
    if (c->dir != CHANNEL_FROM_FD)
        return;

    size_t maxoutmsg = adbx_maxoutmsg(sh);
    size_t avail = ringbuf_size(c->rb);
    struct msg_channel_data m;

    if (maxoutmsg > sizeof (m) && avail > 0) {
        size_t payloadsz = XMIN(avail, maxoutmsg - sizeof (m));
        struct iovec iov[3] = {{ &m, sizeof (m) }};
        ringbuf_iov(c->rb, &iov[1], payloadsz);
        memset(&m, 0, sizeof (m));
        m.msg.type = MSG_CHANNEL_DATA;
        m.channel = chno;
        m.msg.size = iovec_sum(iov, ARRAYSIZE(iov));
        channel_write(sh->ch[TO_PEER], iov, ARRAYSIZE(iov));
        ringbuf_note_removed(c->rb, payloadsz);
    }
}

static void
xmit_eof(struct channel* c,
         unsigned chno,
         struct adbx_sh* sh)
{
    struct msg_channel_close m;

    if (c->fdh == NULL &&
        c->sent_eof == false &&
        ringbuf_size(c->rb) == 0 &&
        adbx_maxoutmsg(sh) >= sizeof (m))
    {
        memset(&m, 0, sizeof (m));
        m.msg.type = MSG_CHANNEL_CLOSE;
        m.msg.size = sizeof (m);
        m.channel = chno;
        channel_write(sh->ch[TO_PEER], &(struct iovec){&m, sizeof (m)}, 1);
        c->sent_eof = true;
    }
}

static void
do_pending_close(struct channel* c)
{
    if (c->dir == CHANNEL_TO_FD &&
        c->fdh != NULL &&
        ringbuf_size(c->rb) == 0 &&
        c->pending_close)
    {
        channel_close(c);
    }
}

static void
pump_io(struct channel** ch, unsigned nrch)
{
    struct pollfd polls[nrch];
    short work = 0;
    for (unsigned chno = 0; chno < nrch; ++chno) {
        polls[chno] = channel_request_poll(ch[chno]);
        work |= polls[chno].events;
    }

    if (work != 0) {
        if (poll(polls, nrch, -1) < 0 && errno != EINTR) {
            die_errno("poll");
        }
    }

    for (unsigned chno = 0; chno < nrch; ++chno)
        if (polls[chno].revents != 0)
            channel_poll(ch[chno]);
}

void
io_loop_1(struct adbx_sh* sh)
{
    SCOPED_RESLIST(rl_io_loop);

    struct channel** ch = sh->ch;
    unsigned chno;
    unsigned nrch = sh->nrch;
    assert(nrch >= NR_SPECIAL_CH);

    pump_io(ch, nrch);
    struct msg mhdr;
    while (detect_msg(ch[FROM_PEER]->rb, &mhdr))
        sh->process_msg(sh, mhdr);

    for (chno = 0; chno < nrch; ++chno)
        xmit_acks(ch[chno], chno, sh);

    for (chno = NR_SPECIAL_CH; chno < nrch; ++chno) {
        xmit_data(ch[chno], chno, sh);
        do_pending_close(ch[chno]);
        xmit_eof(ch[chno], chno, sh);
    }
}

void
queue_message_synch(struct adbx_sh* sh, struct msg* m)
{
    while (adbx_maxoutmsg(sh) < m->size)
        io_loop_1(sh);

    channel_write(sh->ch[TO_PEER], &(struct iovec){m, m->size}, 1);
}