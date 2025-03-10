/*
 * ws80211 utilities
 * Copyright 2012, Pontus Fuchs <pontus.fuchs@gmail.com>

Parts of this file was copied from iw:

Copyright (c) 2007, 2008	Johannes Berg
Copyright (c) 2007		Andy Lutomirski
Copyright (c) 2007		Mike Kershaw
Copyright (c) 2008-2009		Luis R. Rodriguez

SPDX-License-Identifier: ISC
*/

#include "config.h"
#include "ws80211_utils.h"

#include <stdio.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <wsutil/array.h>

#if defined(HAVE_LIBNL) && defined(HAVE_NL80211)
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <net/if.h>
#include <sys/ioctl.h>

#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>
#include <netlink/msg.h>
#include <netlink/attr.h>

#include <linux/nl80211.h>

#ifdef HAVE_NL80211_SPLIT_WIPHY_DUMP
static int ws80211_get_protocol_features(int* features);
#endif /* HAVE_NL80211_SPLIT_WIPHY_DUMP */

/* libnl 1.x compatibility code */
#ifdef HAVE_LIBNL1
#define nl_sock nl_handle
static inline struct nl_handle *nl_socket_alloc(void)
{
	return nl_handle_alloc();
}

static inline void nl_socket_free(struct nl_sock *h)
{
	nl_handle_destroy(h);
}
#endif /* HAVE_LIBNL1 */

struct nl80211_state {
	struct nl_sock *nl_sock;
	int nl80211_id;
	int have_split_wiphy;
};

static struct nl80211_state nl_state;

int ws80211_init(void)
{
	int err;
#ifdef HAVE_NL80211_SPLIT_WIPHY_DUMP
	int features = 0;
#endif /* HAVE_NL80211_SPLIT_WIPHY_DUMP */

	struct nl80211_state *state = &nl_state;

	state->nl_sock = nl_socket_alloc();
	if (!state->nl_sock) {
		fprintf(stderr, "Failed to allocate netlink socket.\n");
		return -ENOMEM;
	}

	if (genl_connect(state->nl_sock)) {
		fprintf(stderr, "Failed to connect to generic netlink.\n");
		err = -ENOLINK;
		goto out_handle_destroy;
	}

	state->nl80211_id = genl_ctrl_resolve(state->nl_sock, "nl80211");
	if (state->nl80211_id < 0) {
		fprintf(stderr, "nl80211 not found.\n");
		err = -ENOENT;
		goto out_handle_destroy;
	}
#ifdef HAVE_NL80211_SPLIT_WIPHY_DUMP
	ws80211_get_protocol_features(&features);
	if (features & NL80211_PROTOCOL_FEATURE_SPLIT_WIPHY_DUMP)
		state->have_split_wiphy = true;
#endif /* HAVE_NL80211_SPLIT_WIPHY_DUMP */

	return WS80211_INIT_OK;

 out_handle_destroy:
	nl_socket_free(state->nl_sock);
	state->nl_sock = 0;
	return err;
}

static int error_handler(struct sockaddr_nl *nla _U_, struct nlmsgerr *err,
			 void *arg)
{
	int *ret = (int *)arg;
	*ret = err->error;
	return NL_STOP;
}

static int finish_handler(struct nl_msg *msg _U_, void *arg)
{
	int *ret = (int *)arg;
	*ret = 0;
	return NL_SKIP;
}

static int ack_handler(struct nl_msg *msg _U_, void *arg)
{
	int *ret = (int *)arg;
	*ret = 0;
	return NL_STOP;
}

static int nl80211_do_cmd(struct nl_msg *msg, struct nl_cb *cb)
{
	/*
	 * XXX - Coverity doesn't understand how libnl works, so it
	 * doesn't know that nl_recvmsgs() calls the callback, and
	 * that the callback has had a pointer to err registered
	 * with it, and therefore that nl_recvmsgs() can change
	 * err as a side-effect, so it thinks this can loop
	 * infinitely.
	 *
	 * The proper way to address this is to help Coverity to
	 * understand the behaviour of nl_recvmsgs(), in that it
	 * does call the callback, setting err. This help would be
	 * provided through a so called 'model' of this function.
	 * We declare err to be volatile to work around it.
	 *
	 * XXX - that workaround provokes a compiler complaint that
	 * casting a pointer to it to "void *" discards the
	 * volatile qualifier.  Perhaps we should just re-close
	 * Coverity CID 997052 as "false positive".
	 */
	volatile int err;

	if (!nl_state.nl_sock)
		return -ENOLINK;

	err = nl_send_auto_complete(nl_state.nl_sock, msg);
	if (err < 0)
		goto out;

	err = 1;

	nl_cb_err(cb, NL_CB_CUSTOM, error_handler, (void *)&err);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, (void *)&err);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, (void *)&err);

	while (err > 0)
		nl_recvmsgs(nl_state.nl_sock, cb);
 out:
	nl_cb_put(cb);

	return err;
}

struct nliface_cookie
{
	char *ifname;
	GArray *interfaces;
};

static struct ws80211_interface *
	get_interface_by_name(GArray *interfaces,
			      char* ifname)
{
	unsigned int i;
	struct ws80211_interface *iface;

	for (i = 0; i < interfaces->len; i++) {
		iface = g_array_index(interfaces, struct ws80211_interface *, i);
		if (!strcmp(iface->ifname, ifname))
			return iface;
	}
	return NULL;
}

#ifdef HAVE_NL80211_SPLIT_WIPHY_DUMP
static int get_features_handler(struct nl_msg *msg, void *arg)
{
	int *feat = (int*) arg;
	struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];
	struct genlmsghdr *gnlh = (struct genlmsghdr *)nlmsg_data(nlmsg_hdr(msg));

	nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);

	if (tb_msg[NL80211_ATTR_PROTOCOL_FEATURES])
		*feat = nla_get_u32(tb_msg[NL80211_ATTR_PROTOCOL_FEATURES]);

	return NL_SKIP;
}

static int ws80211_get_protocol_features(int* features)
{
	struct nl_msg *msg;
	struct nl_cb *cb;
	int ret;

	msg = nlmsg_alloc();
	if (!msg) {
		fprintf(stderr, "failed to allocate netlink message\n");
		return 2;
	}

	cb = nl_cb_alloc(NL_CB_DEFAULT);

	genlmsg_put(msg, 0, 0, nl_state.nl80211_id, 0, 0,
		    NL80211_CMD_GET_PROTOCOL_FEATURES, 0);

	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, get_features_handler, features);

	ret = nl80211_do_cmd(msg, cb);
	nlmsg_free(msg);
	return ret;
}
#endif /* HAVE_NL80211_SPLIT_WIPHY_DUMP */

#ifdef NL80211_BAND_ATTR_HT_CAPA
static void parse_band_ht_capa(struct ws80211_interface *iface,
			       struct nlattr *tb)
{
	bool ht40;

	if (!tb) return;

	iface->channel_types |= 1 << WS80211_CHAN_HT20;
	ht40 = !!(nla_get_u16(tb) & 0x02);
	if (ht40) {
		iface->channel_types |= 1 << WS80211_CHAN_HT40MINUS;
		iface->channel_types |= 1 << WS80211_CHAN_HT40PLUS;
	}
}
#endif /* NL80211_BAND_ATTR_HT_CAPA */

#ifdef HAVE_NL80211_VHT_CAPABILITY
static void parse_band_vht_capa(struct ws80211_interface *iface,
				struct nlattr *tb)
{
	uint32_t chan_capa;
	if (!tb) return;

	chan_capa = (nla_get_u32(tb) >> 2) & 3;
	if (chan_capa == 1) {
		iface->channel_types |= 1 << WS80211_CHAN_VHT160;
	}
	if (chan_capa == 2) {
		iface->channel_types |= 1 << WS80211_CHAN_VHT160;
		iface->channel_types |= 1 << WS80211_CHAN_VHT80P80;
	}
	iface->channel_types |= 1 << WS80211_CHAN_VHT80;
}
#endif /* HAVE_NL80211_VHT_CAPABILITY */

static void parse_supported_iftypes(struct ws80211_interface *iface,
				    struct nlattr *tb)
{
	struct nlattr *nl_mode;
	int rem_mode;

	if (!tb) return;

	nla_for_each_nested(nl_mode, tb, rem_mode) {
		if (nla_type(nl_mode) == NL80211_IFTYPE_MONITOR)
			iface->cap_monitor = 1;
	}
}

static void parse_band_freqs(struct ws80211_interface *iface,
			     struct nlattr *tb)
{
	struct nlattr *nl_freq;
	struct nlattr *tb_freq[NL80211_FREQUENCY_ATTR_MAX + 1];
	static struct nla_policy freq_policy[NL80211_FREQUENCY_ATTR_MAX + 1] = {
		{NLA_UNSPEC, 0, 0},		/* __NL80211_FREQUENCY_ATTR_INVALID */
		{NLA_U32, 0, 0},		/* NL80211_FREQUENCY_ATTR_FREQ */
		{NLA_FLAG, 0, 0},		/* NL80211_FREQUENCY_ATTR_DISABLED */
		{NLA_FLAG, 0, 0},		/* NL80211_FREQUENCY_ATTR_PASSIVE_SCAN */
		{NLA_FLAG, 0, 0},		/* NL80211_FREQUENCY_ATTR_NO_IBSS */
		{NLA_FLAG, 0, 0},		/* NL80211_FREQUENCY_ATTR_RADAR */
		{NLA_U32, 0, 0}			/* NL80211_FREQUENCY_ATTR_MAX_TX_POWER */
	};
	int rem_freq;

	if (!tb) return;

	nla_for_each_nested(nl_freq, tb, rem_freq) {
		uint32_t freq;
		nla_parse(tb_freq, NL80211_FREQUENCY_ATTR_MAX,
			  (struct nlattr *)nla_data(nl_freq),
			  nla_len(nl_freq), freq_policy);
		if (!tb_freq[NL80211_FREQUENCY_ATTR_FREQ])
			continue;
		if (tb_freq[NL80211_FREQUENCY_ATTR_DISABLED])
			continue;

		freq = nla_get_u32(tb_freq[NL80211_FREQUENCY_ATTR_FREQ]);
		g_array_append_val(iface->frequencies, freq);
	}
}

static void parse_wiphy_bands(struct ws80211_interface *iface,
			     struct nlattr *tb)
{
	struct nlattr *nl_band;
	struct nlattr *tb_band[NL80211_BAND_ATTR_MAX + 1];
	int rem_band;

	if (!tb) return;

	nla_for_each_nested(nl_band, tb, rem_band) {
		nla_parse(tb_band, NL80211_BAND_ATTR_MAX,
			  (struct nlattr *)nla_data(nl_band),
			  nla_len(nl_band), NULL);

#ifdef NL80211_BAND_ATTR_HT_CAPA
		parse_band_ht_capa(iface, tb_band[NL80211_BAND_ATTR_HT_CAPA]);
#endif /* NL80211_BAND_ATTR_HT_CAPA */
#ifdef HAVE_NL80211_VHT_CAPABILITY
		parse_band_vht_capa(iface, tb_band[NL80211_BAND_ATTR_VHT_CAPA]);
#endif /* HAVE_NL80211_VHT_CAPABILITY */
		parse_band_freqs(iface, tb_band[NL80211_BAND_ATTR_FREQS]);
	}
}

static void parse_supported_commands(struct ws80211_interface *iface,
				     struct nlattr *tb)
{
	/* Can frequency be set? Only newer versions of cfg80211 supports this */
#ifdef HAVE_NL80211_CMD_SET_CHANNEL
	int cmd;
	struct nlattr *nl_cmd;

	if (!tb) return;

	nla_for_each_nested(nl_cmd, tb, cmd) {
		if(nla_get_u32(nl_cmd) == NL80211_CMD_SET_CHANNEL)
			iface->can_set_freq = true;
	}
#else
	iface->can_set_freq = true;
#endif
}

static int get_phys_handler(struct nl_msg *msg, void *arg)
{
	struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];
	struct genlmsghdr *gnlh = (struct genlmsghdr *)nlmsg_data(nlmsg_hdr(msg));

	struct nliface_cookie *cookie = (struct nliface_cookie *)arg;

	struct ws80211_interface *iface;
	char* ifname;
	int added = 0;

	nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);

	if (!tb_msg[NL80211_ATTR_WIPHY_NAME])
		return NL_SKIP;

	ifname = ws_strdup_printf("%s.mon", nla_get_string(tb_msg[NL80211_ATTR_WIPHY_NAME]));
	iface = get_interface_by_name(cookie->interfaces, ifname);

	if (!iface) {
		iface = (struct ws80211_interface *)g_malloc0(sizeof(*iface));
		if (!iface) {
			g_free(ifname);
			return NL_SKIP;
		}
		added = 1;
		iface->ifname = ifname;
		iface->frequencies = g_array_new(false, false, sizeof(uint32_t));
		iface->channel_types = 1 << WS80211_CHAN_NO_HT;
	} else {
		g_free(ifname);
	}

	parse_supported_iftypes(iface, tb_msg[NL80211_ATTR_SUPPORTED_IFTYPES]);
	parse_wiphy_bands(iface, tb_msg[NL80211_ATTR_WIPHY_BANDS]);
	parse_supported_commands(iface, tb_msg[NL80211_ATTR_SUPPORTED_COMMANDS]);

	if (added)
		g_array_append_val(cookie->interfaces, iface);

	return NL_SKIP;
}

static int ws80211_get_phys(GArray *interfaces)
{
	struct nliface_cookie cookie;
	struct nl_msg *msg;
	struct nl_cb *cb;
	int ret;
	msg = nlmsg_alloc();
	if (!msg) {
		fprintf(stderr, "failed to allocate netlink message\n");
		return 2;
	}

	cb = nl_cb_alloc(NL_CB_DEFAULT);

	cookie.interfaces = interfaces;

	genlmsg_put(msg, 0, 0, nl_state.nl80211_id, 0,
		    NLM_F_DUMP, NL80211_CMD_GET_WIPHY, 0);

#ifdef HAVE_NL80211_SPLIT_WIPHY_DUMP
	if (nl_state.have_split_wiphy) {
		NLA_PUT_FLAG(msg, NL80211_ATTR_SPLIT_WIPHY_DUMP);
	}
#endif /* #ifdef HAVE_NL80211_SPLIT_WIPHY_DUMP */
	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, get_phys_handler, &cookie);

	ret = nl80211_do_cmd(msg, cb);
	nlmsg_free(msg);
	return ret;

#ifdef HAVE_NL80211_SPLIT_WIPHY_DUMP
nla_put_failure:
	nlmsg_free(msg);
	fprintf(stderr, "building message failed\n");
	return -1;
#endif /* HAVE_NL80211_SPLIT_WIPHY_DUMP */
}

static int get_freq_wext(const char *ifname)
{
	int fd;
	int ret = -1;
	/* Ugly hack to avoid including wireless.h */
	struct {
		char name1[IFNAMSIZ];
		__s32 m;
		__s16 e;
		__u8 i;
		__u8 flags;
	} wrq;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd == -1)
		return -1;

	(void) g_strlcpy(wrq.name1, ifname, IFNAMSIZ);
	/* SIOCGIWFREQ */
	if (ioctl(fd, 0x8B05, &wrq) == 0) {
		if (wrq.e == 6)
			ret = wrq.m;
	}
	close(fd);
	return ret;
}

struct __iface_info
{
	struct ws80211_iface_info *pub;
	int type;
	int phyidx;
};

static int get_iface_info_handler(struct nl_msg *msg, void *arg)
{
	struct genlmsghdr *gnlh = (struct genlmsghdr *)nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];
	struct __iface_info *iface_info = (struct __iface_info *)arg;

	nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);

	if (tb_msg[NL80211_ATTR_IFTYPE]) {
		iface_info->type = nla_get_u32(tb_msg[NL80211_ATTR_IFTYPE]);
	}
	if (tb_msg[NL80211_ATTR_WIPHY]) {
		iface_info->phyidx = nla_get_u32(tb_msg[NL80211_ATTR_WIPHY]);
	}

	if (tb_msg[NL80211_ATTR_WIPHY_FREQ]) {
		bool found_ch_width = false;
		iface_info->pub->current_freq = nla_get_u32(tb_msg[NL80211_ATTR_WIPHY_FREQ]);
		iface_info->pub->current_chan_type = WS80211_CHAN_NO_HT;
#ifdef HAVE_NL80211_VHT_CAPABILITY
		if (tb_msg[NL80211_ATTR_CHANNEL_WIDTH]) {
			switch (nla_get_u32(tb_msg[NL80211_ATTR_CHANNEL_WIDTH])) {
			case NL80211_CHAN_WIDTH_80:
				iface_info->pub->current_chan_type = WS80211_CHAN_VHT80;
				found_ch_width = true;
				break;
			case NL80211_CHAN_WIDTH_80P80:
				iface_info->pub->current_chan_type = WS80211_CHAN_VHT80P80;
				found_ch_width = true;
				break;
			case NL80211_CHAN_WIDTH_160:
				iface_info->pub->current_chan_type = WS80211_CHAN_VHT160;
				found_ch_width = true;
				break;
			}
		}
		if (tb_msg[NL80211_ATTR_CENTER_FREQ1]) {
			iface_info->pub->current_center_freq1 =
				nla_get_u32(tb_msg[NL80211_ATTR_CENTER_FREQ1]);
		}
		if (tb_msg[NL80211_ATTR_CENTER_FREQ2]) {
			iface_info->pub->current_center_freq2 =
				nla_get_u32(tb_msg[NL80211_ATTR_CENTER_FREQ2]);
		}
#endif
		if (!found_ch_width && tb_msg[NL80211_ATTR_WIPHY_CHANNEL_TYPE]) {
			switch (nla_get_u32(tb_msg[NL80211_ATTR_WIPHY_CHANNEL_TYPE])) {

			case NL80211_CHAN_NO_HT:
				iface_info->pub->current_chan_type = WS80211_CHAN_NO_HT;
				break;

			case NL80211_CHAN_HT20:
				iface_info->pub->current_chan_type = WS80211_CHAN_HT20;
				break;

			case NL80211_CHAN_HT40MINUS:
				iface_info->pub->current_chan_type = WS80211_CHAN_HT40MINUS;
				break;

			case NL80211_CHAN_HT40PLUS:
				iface_info->pub->current_chan_type = WS80211_CHAN_HT40PLUS;
				break;
			}
		}

	}
	return NL_SKIP;
}


static int __ws80211_get_iface_info(const char *name, struct __iface_info *iface_info)
{
	int devidx;
	struct nl_msg *msg;
	struct nl_cb *cb;
	msg = nlmsg_alloc();
	if (!msg) {
		fprintf(stderr, "failed to allocate netlink message\n");
		return 2;
	}

	cb = nl_cb_alloc(NL_CB_DEFAULT);

	devidx = if_nametoindex(name);

	genlmsg_put(msg, 0, 0, nl_state.nl80211_id, 0,
		    0, NL80211_CMD_GET_INTERFACE, 0);
	NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);

	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, get_iface_info_handler, iface_info);

	if (nl80211_do_cmd(msg, cb)) {
		nlmsg_free(msg);
		return -1;
	}

	/* Old kernels can't get the current freq via netlink. Try WEXT too :( */
	if (iface_info->pub->current_freq == -1)
		iface_info->pub->current_freq = get_freq_wext(name);
	nlmsg_free(msg);
	return 0;

nla_put_failure:
	nlmsg_free(msg);
	fprintf(stderr, "building message failed\n");
	return -1;
}

int ws80211_get_iface_info(const char *name, struct ws80211_iface_info *iface_info)
{
	struct __iface_info __iface_info;

	memset(iface_info, 0, sizeof(*iface_info));
	__iface_info.pub = iface_info;
	__iface_info.type = -1;
	__iface_info.phyidx= -1;
	__iface_info.pub->current_freq = -1;
	__iface_info.pub->current_chan_type = WS80211_CHAN_NO_HT;

	return __ws80211_get_iface_info(name, &__iface_info);
}

static int ws80211_keep_only_monitor(GArray *interfaces)
{
	unsigned int j;
	struct ws80211_interface *iface;
restart:
	for (j = 0; j < interfaces->len; j++) {
		iface = g_array_index(interfaces, struct ws80211_interface *, j);
		if (!iface->cap_monitor) {
			g_array_remove_index(interfaces, j);
			g_array_free(iface->frequencies, true);
			g_free(iface->ifname);
			g_free(iface);
			goto restart;
		}
	}
	return 0;
}

static int ws80211_populate_devices(GArray *interfaces)
{
	FILE *fh;
	char line[200];
	char *t;
	char *t2;
	char *ret;
	int i;
	unsigned int j;

	struct ws80211_iface_info pub = {-1, WS80211_CHAN_NO_HT, -1, -1, WS80211_FCS_ALL};
	struct __iface_info iface_info;
	struct ws80211_interface *iface;

	/* Get a list of phy's that can handle monitor mode */
	ws80211_get_phys(interfaces);
	ws80211_keep_only_monitor(interfaces);

	fh = g_fopen("/proc/net/dev", "r");
	if(!fh) {
		fprintf(stderr, "Cannot open /proc/net/dev");
		return -ENOENT;
	}

	/* Skip the first two lines */
	for (i = 0; i < 2; i++) {
		ret = fgets(line, sizeof(line), fh);
		if (ret == NULL) {
			fprintf(stderr, "Error parsing /proc/net/dev");
			fclose(fh);
			return -1;
		}
	}

	/* Update names of user created monitor interfaces */
	while(fgets(line, sizeof(line), fh)) {
		t = index(line, ':');
		if (!t)
			continue;
		*t = 0;
		t = line;
		while (*t == ' ')
			t++;
		memset(&iface_info, 0, sizeof(iface_info));
		iface_info.pub = &pub;
		__ws80211_get_iface_info(t, &iface_info);

		if (iface_info.type == NL80211_IFTYPE_MONITOR) {
			for (j = 0; j < interfaces->len; j++) {
				iface = g_array_index(interfaces, struct ws80211_interface *, j);
				t2 = ws_strdup_printf("phy%d.mon", iface_info.phyidx);
				if (t2) {
					if (!strcmp(t2, iface->ifname)) {
						g_free(iface->ifname);
						iface->ifname = g_strdup(t);
					}
					g_free(t2);
				}
			}
		}
	}
	fclose(fh);
	return 0;
}

static int ws80211_iface_up(const char *ifname)
{
	int sock;
	struct ifreq ifreq;

	sock = socket(AF_PACKET, SOCK_RAW, 0);
	if (sock == -1)
		return -1;

	(void) g_strlcpy(ifreq.ifr_name, ifname, sizeof(ifreq.ifr_name));

	if (ioctl(sock, SIOCGIFFLAGS, &ifreq))
		goto out_err;

	ifreq.ifr_flags |= IFF_UP;

	if (ioctl(sock, SIOCSIFFLAGS, &ifreq))
		goto out_err;

	close(sock);
	return 0;

out_err:
	close(sock);
	return -1;
}

/* Needed for NLA_PUT_STRING, which passes strlen as an int */
DIAG_OFF_CLANG(shorten-64-to-32)
static int ws80211_create_on_demand_interface(const char *name)
{
	int devidx, phyidx, err;
	struct nl_msg *msg;
	struct nl_cb *cb;

	devidx = if_nametoindex(name);
	if (devidx)
		return ws80211_iface_up(name);

	if (sscanf(name, "phy%d.mon", &phyidx) != 1)
		return -EINVAL;

	cb = nl_cb_alloc(NL_CB_DEFAULT);
	msg = nlmsg_alloc();
	if (!msg) {
		fprintf(stderr, "failed to allocate netlink message\n");
		return 2;
	}

	genlmsg_put(msg, 0, 0, nl_state.nl80211_id, 0,
		    0, NL80211_CMD_NEW_INTERFACE, 0);
	NLA_PUT_U32(msg, NL80211_ATTR_WIPHY, phyidx);

	NLA_PUT_STRING(msg, NL80211_ATTR_IFNAME, name);
	NLA_PUT_U32(msg, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_MONITOR);

	err = nl80211_do_cmd(msg, cb);
	nlmsg_free(msg);
	if (err)
		return err;
	return ws80211_iface_up(name);

nla_put_failure:
	nlmsg_free(msg);
	fprintf(stderr, "building message failed\n");
	return 2;
}
DIAG_ON_CLANG(shorten-64-to-32)

int ws80211_set_freq(const char *name, uint32_t freq, int chan_type, uint32_t _U_ center_freq, uint32_t _U_ center_freq2)
{
	int devidx, err;
	struct nl_msg *msg;
	struct nl_cb *cb;

	err = ws80211_create_on_demand_interface(name);
	if (err)
		return err;

	msg = nlmsg_alloc();
	if (!msg) {
		fprintf(stderr, "failed to allocate netlink message\n");
		return 2;
	}

	cb = nl_cb_alloc(NL_CB_DEFAULT);

	devidx = if_nametoindex(name);

#ifdef HAVE_NL80211_CMD_SET_CHANNEL
	genlmsg_put(msg, 0, 0, nl_state.nl80211_id, 0,
		    0, NL80211_CMD_SET_CHANNEL, 0);
#else
	genlmsg_put(msg, 0, 0, nl_state.nl80211_id, 0,
		    0, NL80211_CMD_SET_WIPHY, 0);
#endif

	NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
	NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_FREQ, freq);

	switch (chan_type) {

#ifdef NL80211_BAND_ATTR_HT_CAPA
	case WS80211_CHAN_NO_HT:
		NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_NO_HT);
		break;

	case WS80211_CHAN_HT20:
		NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_HT20);
		break;

	case WS80211_CHAN_HT40MINUS:
		NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_HT40MINUS);
		break;

	case WS80211_CHAN_HT40PLUS:
		NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_HT40PLUS);
		break;
#endif
#ifdef HAVE_NL80211_VHT_CAPABILITY
	case WS80211_CHAN_VHT80:
		NLA_PUT_U32(msg, NL80211_ATTR_CHANNEL_WIDTH, NL80211_CHAN_WIDTH_80);
		NLA_PUT_U32(msg, NL80211_ATTR_CENTER_FREQ1, center_freq);
		break;

	case WS80211_CHAN_VHT80P80:
		NLA_PUT_U32(msg, NL80211_ATTR_CHANNEL_WIDTH, NL80211_CHAN_WIDTH_80P80);
		NLA_PUT_U32(msg, NL80211_ATTR_CENTER_FREQ1, center_freq);
		NLA_PUT_U32(msg, NL80211_ATTR_CENTER_FREQ2, center_freq2);
		break;

	case WS80211_CHAN_VHT160:
		NLA_PUT_U32(msg, NL80211_ATTR_CHANNEL_WIDTH, NL80211_CHAN_WIDTH_160);
		NLA_PUT_U32(msg, NL80211_ATTR_CENTER_FREQ1, center_freq);
		break;
#endif
	default:
		break;
	}
	err = nl80211_do_cmd(msg, cb);
	nlmsg_free(msg);
	return err;

nla_put_failure:
	nlmsg_free(msg);
	fprintf(stderr, "building message failed\n");
	return 2;

}

GArray* ws80211_find_interfaces(void)
{
	GArray *interfaces;

	if (!nl_state.nl_sock)
		return NULL;

	interfaces = g_array_new(false, false, sizeof(struct ws80211_interface *));
	if (!interfaces)
		return NULL;

	if (ws80211_populate_devices(interfaces)) {
		ws80211_free_interfaces(interfaces);
		return NULL;
	}
	return interfaces;
}

int
ws80211_str_to_chan_type(const char *s)
{
	int ret = -1;
	if (!s)
		return -1;

	if (!strcmp(s, CHAN_NO_HT))
		ret = WS80211_CHAN_NO_HT;
	if (!strcmp(s, CHAN_HT20))
		ret = WS80211_CHAN_HT20;
	if (!strcmp(s, CHAN_HT40MINUS))
		ret = WS80211_CHAN_HT40MINUS;
	if (!strcmp(s, CHAN_HT40PLUS))
		ret = WS80211_CHAN_HT40PLUS;
	if (!strcmp(s, CHAN_VHT80))
		ret = WS80211_CHAN_VHT80;
	if (!strcmp(s, CHAN_VHT80P80))
		ret = WS80211_CHAN_VHT80P80;
	if (!strcmp(s, CHAN_VHT160))
		ret = WS80211_CHAN_VHT160;

	return ret;
}

const char
*ws80211_chan_type_to_str(int type)
{
	switch (type) {
	case WS80211_CHAN_NO_HT:
		return CHAN_NO_HT;
	case WS80211_CHAN_HT20:
		return CHAN_HT20;
	case WS80211_CHAN_HT40MINUS:
		return CHAN_HT40MINUS;
	case WS80211_CHAN_HT40PLUS:
		return CHAN_HT40PLUS;
	case WS80211_CHAN_VHT80:
		return CHAN_VHT80;
	case WS80211_CHAN_VHT80P80:
		return CHAN_VHT80P80;
	case WS80211_CHAN_VHT160:
		return CHAN_VHT160;
	}
	return NULL;
}

bool ws80211_has_fcs_filter(void)
{
	return false;
}

int ws80211_set_fcs_validation(const char *name _U_, enum ws80211_fcs_validation fcs_validation _U_)
{
	return -1;
}

const char *network_manager_path = "/usr/sbin/NetworkManager"; /* Is this correct? */
const char *ws80211_get_helper_path(void) {
	if (g_file_test(network_manager_path, G_FILE_TEST_IS_EXECUTABLE)) {
		return network_manager_path;
	}
	return NULL;
}

#else /* Everyone else. */
int ws80211_init(void)
{
	return WS80211_INIT_NOT_SUPPORTED;
}

GArray* ws80211_find_interfaces(void)
{
	return NULL;
}

int ws80211_get_iface_info(const char *name _U_, struct ws80211_iface_info *iface_info _U_)
{
	return -1;
}

int ws80211_set_freq(const char *name _U_, uint32_t freq _U_, int _U_ chan_type, uint32_t _U_ center_freq, uint32_t _U_ center_freq2)
{
	return -1;
}

int ws80211_str_to_chan_type(const char *s _U_)
{
	return -1;
}

const char *ws80211_chan_type_to_str(int type _U_)
{
	return NULL;
}

bool ws80211_has_fcs_filter(void)
{
	return false;
}

int ws80211_set_fcs_validation(const char *name _U_, enum ws80211_fcs_validation fcs_validation _U_)
{
	return -1;
}

const char *ws80211_get_helper_path(void) {
	return NULL;
}

#endif /* HAVE_LIBNL && HAVE_NL80211 */

/* Common to everyone */

void ws80211_free_interfaces(GArray *interfaces)
{
	struct ws80211_interface *iface;

	if (!interfaces)
		return;

	while (interfaces->len) {
		iface = g_array_index(interfaces, struct ws80211_interface *, 0);
		g_array_remove_index(interfaces, 0);
		g_array_free(iface->frequencies, true);
		g_free(iface->ifname);
		g_free(iface);
	}
	g_array_free(interfaces, true);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 noexpandtab:
 * :indentSize=8:tabSize=8:noTabs=false:
 */
