/*
 * netmap interface for unetstack
 *
 * BSD license
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"
#include <errno.h>
#include <sys/poll.h>

#ifdef USE_PCAP
#include <pcap/pcap.h>
struct pcap *my_pcap = NULL;
#endif /* USE_PCAP */

#ifdef NETMAP
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <net/if.h>
#include <net/netmap.h>
#include <net/netmap_user.h>
#endif

int verbose;

#ifdef NETMAP
struct my_ring {
        struct nmreq nmr;

        int fd;
        char *mem;                      /* userspace mmap address */
        u_int memsize;
        u_int queueid;
        u_int begin, end;               /* first..last+1 rings to check */
        struct netmap_if *nifp;
  
        int snaplen;
        char *errbuf;
        int promisc;
        int to_ms;
        
        uint32_t if_flags;
        uint32_t if_reqcap;
        uint32_t if_curcap;
 
        char msg[128];
};

        
static int
do_ioctl(struct my_ring *me, int what)
{
        struct ifreq ifr;
        int error;
        
        bzero(&ifr, sizeof(ifr));
        strncpy(ifr.ifr_name, me->nmr.nr_name, sizeof(ifr.ifr_name));
        switch (what) {
        case SIOCSIFFLAGS:
                D("call SIOCSIFFLAGS 0x%x", me->if_flags);
                ifr.ifr_flagshigh = (me->if_flags >> 16) & 0xffff;
                ifr.ifr_flags = me->if_flags & 0xffff;
                break;
        case SIOCSIFCAP:
                ifr.ifr_reqcap = me->if_reqcap;
                ifr.ifr_curcap = me->if_curcap;
                break;
        }
        error = ioctl(me->fd, what, &ifr);
        if (error) {
                D("ioctl 0x%x error %d", what, error);
                return error;
        }
        switch (what) {
        case SIOCSIFFLAGS:
        case SIOCGIFFLAGS:
                me->if_flags = (ifr.ifr_flagshigh << 16) |
                        (0xffff & ifr.ifr_flags);
                D("flags are L 0x%x H 0x%x 0x%x",
                        (uint16_t)ifr.ifr_flags,
                        (uint16_t)ifr.ifr_flagshigh, me->if_flags);
                break;

        case SIOCGIFCAP:
                me->if_reqcap = ifr.ifr_reqcap;
                me->if_curcap = ifr.ifr_curcap;
                D("curcap are 0x%x", me->if_curcap);
                break;
        }
        return 0;
}
#endif /* NETMAP */


/*
 * allocate a buffer and data
 */
struct nc_buff *
ncb_alloc(unsigned int size)
{
        struct nc_buff *ncb;

        ncb = malloc(sizeof(*ncb) + size);
        if (!ncb)
                return NULL;

        memset(ncb, 0, sizeof(struct nc_buff));

	ncb->data = ncb->head = (void *)(ncb + 1);
        ncb->len = ncb->total_size = size;

        ncb_timestamp(&ncb->tstamp);
        ncb->refcnt = 1;
        ncb->tail = ncb->end = ncb->head + ncb->len;

        return ncb;
}

void ncb_free(struct nc_buff *ncb)
{
#if 0
        if (ncb->dst)
                route_put(ncb->dst);
#endif
        memset(ncb, 0xFF, sizeof(struct nc_buff));
        free(ncb);
}


char *ifname = "eth0";
unsigned char packet_edst[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};


int netchannel_send_raw(struct nc_buff *ncb)
{
#ifdef USE_PCAP
	return pcap_inject(my_pcap, ncb->head, ncb->len);
#endif /* USE_PCAP */
	//sendto(ncb->nc->fd, ncb->head, ncb->len);

	return 0;
}

#ifdef USE_PCAP
static void
my_pcap_cb(u_char *d, const  struct  pcap_pkthdr *hdr, const u_char *snap)
{
	struct nc_buff *ncb = (struct nc_buff *)d;
	bcopy(snap, ncb->head, hdr->caplen);
	ncb_trim(ncb, hdr->caplen);
	packet_ip_process(ncb);
}
#endif
int netchannel_recv_raw(struct netchannel *nc, unsigned int tm)
{
	struct nc_buff *ncb;
	int err;
	struct pollfd pfd;

	pfd.fd = nc->fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	syscall_recv += 1;

	err = poll(&pfd, 1, tm);
	if (err < 0) {
		ulog_err("%s: failed to poll", __func__);
		return err;
	}
	if (!(pfd.revents & POLLIN) || !err) {
		ulog("%s: no data, revents: %x.\n", __func__, pfd.revents);
		return -EAGAIN;
	}

	syscall_recv += 1;

	ncb = ncb_alloc(4096);
	if (!ncb)
		return -ENOMEM;
	ncb->nc = nc;
	pcap_dispatch(my_pcap, 50, my_pcap_cb, (u_char *)ncb);
	ncb_put(ncb);
	return 0;

}

static int netchannel_create_raw(struct netchannel *nc __unused)
{
#ifdef USE_PCAP
	char errbuff[PCAP_ERRBUF_SIZE];
	my_pcap = pcap_open_live(ifname, 0, 1, 100, errbuff);
	return (my_pcap ? pcap_fileno(my_pcap) : -1);

#endif
#ifdef NETMAP
#endif
	return 0;
}

#ifdef NETMAP
int
netmap_open(struct my_ring *me, int ringid)
{
        int fd, err, l;
        u_int i;
        struct nmreq req;

        me->fd = fd = open("/dev/netmap", O_RDWR);
        if (fd < 0) {
                D("Unable to open /dev/netmap");
                return (-1);
        }
        bzero(&req, sizeof(req));
        strncpy(req.nr_name, me->nmr.nr_name, sizeof(req.nr_name));
        req.nr_ringid = ringid;
        err = ioctl(fd, NIOCGINFO, &req);
        if (err) {
                D("cannot get info on %s", me->nmr.nr_name);
                goto error;
        }
        me->memsize = l = req.nr_memsize;
        ND("memsize is %d MB", l>>20);
        err = ioctl(fd, NIOCREGIF, &req);
        if (err) {
                D("Unable to register %s", me->nmr.nr_name);
                goto error;
        }

        if (me->mem == NULL) {
                me->mem = mmap(0, l, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
                if (me->mem == MAP_FAILED) {
                        D("Unable to mmap");
                        me->mem = NULL;
                        goto error;
                }
        }

        me->nifp = NETMAP_IF(me->mem, req.nr_offset);
        me->queueid = ringid;
        if (ringid & NETMAP_SW_RING) {
                me->begin = req.nr_rx_rings;
                me->end = me->begin + 1;
        } else if (ringid & NETMAP_HW_RING) {
                me->begin = ringid & NETMAP_RING_MASK;
                me->end = me->begin + 1;
        } else {
                me->begin = 0;
                me->end = req.nr_rx_rings;
        }
        /* request timestamps for packets */
        for (i = me->begin; i < me->end; i++) {
                struct netmap_ring *ring = NETMAP_RXRING(me->nifp, i);
                ring->flags = NR_TIMESTAMP;
        }
        //me->tx = NETMAP_TXRING(me->nifp, 0);
        return (0);
error:
        close(me->fd);
        return -1;
}
#endif /* NETMAP */



void netchannel_remove(struct netchannel *nc)
{
	close(nc->fd);
}

struct netchannel *netchannel_create(struct netchannel_control *ctl, unsigned int state)
{
	int err;
	struct common_protocol *proto;
	struct netchannel *nc;

	if (ctl->saddr.proto == IPPROTO_TCP)
		proto = &atcp_common_protocol;
	else if (ctl->saddr.proto == IPPROTO_UDP)
		proto = &udp_common_protocol;
	else
		return NULL;

	nc = malloc(sizeof(struct netchannel) + proto->size);
	if (!nc)
		return NULL;

	memset(nc, 0, sizeof(struct netchannel) + proto->size);
	ncb_queue_init(&nc->recv_queue);

	nc->proto = (struct common_protocol *)(nc + 1);
	nc->state = state;
	nc->header_size = MAX_HEADER_SIZE;

	memcpy(nc->proto, proto, sizeof(struct common_protocol));
	memcpy(&nc->ctl, ctl, sizeof(struct netchannel_control));

	nc->fd = netchannel_create_raw(nc);
	if (nc->fd < 0) {
		ulog_err("Failed to create netchannel");
		goto err_out_free;
	}

	err = nc->proto->create(nc);
	if (err)
		goto err_out_free;

	return nc;

err_out_free:
	free(nc);
	return NULL;
}

int netchannel_send(struct netchannel *nc, void *buf, unsigned int size)
{
	return nc->proto->process_out(nc, buf, size);
}

int netchannel_recv(struct netchannel *nc, void *buf, unsigned int size)
{
	return nc->proto->process_in(nc, buf, size);
}