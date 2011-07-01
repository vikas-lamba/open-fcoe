/*
 * Copyright(c) 2010-2011 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Maintained at www.Open-FCoE.org
 */

#include <ctype.h>
#include <getopt.h>
#include <malloc.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <libgen.h>
#include <ulimit.h>
#include <unistd.h>
#include <paths.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <netlink/netlink.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/dcbnl.h>

#include <lldpad/dcb_types.h>
#include <lldpad/clif.h>
#include <lldpad/lldp_dcbx_cmds.h>

#include "scsi_netlink_fc.h"
#include "fcoe_utils_version.h"
#include "fcoemon_utils.h"
#include "fcoemon.h"
#include "fcoe_clif.h"
#include "fcoe_utils.h"
#include "hbaapi.h"
#include "strarr.h"

#include "fip.h"
#include "rtnetlink.h"

#ifndef SYSCONFDIR
#define SYSCONFDIR                  "/etc"
#endif

#define CONFIG_DIR                  SYSCONFDIR "/fcoe"
#define CONFIG_MIN_VAL_LEN          (1 + 2)
#define CONFIG_MAX_VAL_LEN          (20 + 2)
#define DCB_APP_0_DEFAULT_ENABLE    1
#define DCB_APP_0_DEFAULT_WILLING   1
#define FILE_NAME_LEN               (NAME_MAX + 1)
#define CFG_FILE_PREFIX             "cfg-"
#define DEF_CFG_FILE                CFG_FILE_PREFIX "ethx"

#define VLAN_DIR                "/proc/net/vlan"

#define CLIF_LOCAL_SUN_PATH     "%s/%d"
#define DCBD_CONNECT_TIMEOUT    (10 * 1000 * 1000)	/* 10 seconds */
#define DCBD_CONNECT_RETRY_TIMEOUT   (1 * 1000 * 1000)	/* 1 seconds */
#define DCBD_REQ_RETRY_TIMEOUT  (200 * 1000)            /* 0.2 seconds */
#define DCBD_MAX_REQ_RETRIES    10
#define FCM_PING_REQ_LEN	1 /* byte-length of dcbd PING request */
#define FCM_PING_RSP_LEN	8 /* byte-length of dcbd PING response */

#define FCOE_CREATE	SYSFS_FCOE "/create"
#define FCOE_DESTROY	SYSFS_FCOE "/destroy"
#define FCOE_ENABLE 	SYSFS_FCOE "/enable"
#define FCOE_DISABLE	SYSFS_FCOE "/disable"

#define FCM_VLAN_DISC_TIMEOUT	(1000 * 1000)	/* 1 seconds */
#define FCM_VLAN_DISC_MAX	10		/* stop after 10 attempts */

#define DEF_RX_BUF_SIZE		4096

#define NLA_DATA(nla)        ((void *)((char *)(nla) + NLA_HDRLEN))
#define NLA_NEXT(nla) (struct rtattr *)((char *)nla + NLMSG_ALIGN(nla->rta_len))

#define FCOE_ETH_TYPE	0x8906

void fcm_vlan_disc_timeout(void *arg);

/*
 * fcoe service configuration data
 * Note: These information are read in from the fcoe service
 *       files in CONFIG_DIR
 */
struct fcoe_port {
	struct fcoe_port *next;

	/* information from fcoe configuration files in CONFIG_DIR */
	char ifname[IFNAMSIZ];       /* netif on which fcoe i/f is created */
	char real_ifname[IFNAMSIZ];  /* underlying net ifname - e.g. if ifname
					is a VLAN */
	int fcoe_enable;
	int dcb_required;
	int auto_vlan;
	int auto_created;

	/* following track data required to manage FCoE interface state */
	enum fcp_action action;      /* current state */
	enum fcp_action last_action; /* last action */
	int last_msg_type;     /* last rtnetlink msg type received on if name */
	struct sock_info *sock_reply;

	int ifindex;
	unsigned char mac[ETHER_ADDR_LEN];
	struct sa_timer vlan_disc_timer;
	int vlan_disc_count;
	int fip_socket;
	char fchost[FCHOSTBUFLEN];
	uint32_t last_fc_event_num;
};

enum fcoeport_ifname {
	FCP_CFG_IFNAME = 0,
	FCP_REAL_IFNAME
};

/*
 * Interact with DCB daemon.
 */
static void fcm_dcbd_timeout(void *);
static void fcm_dcbd_retry_timeout(void *);
static void fcm_dcbd_disconnect(void);
static int fcm_dcbd_request(char *);
static void fcm_dcbd_rx(void *);
static void fcm_dcbd_event(char *, size_t);
static void fcm_dcbd_cmd_resp(char *, cmd_status);
static void fcm_netif_advance(struct fcm_netif *);
static void fcm_fcoe_action(struct fcm_netif *, struct fcoe_port *);
static void fcp_set_next_action(struct fcoe_port *, enum fcp_action);
static enum fcoe_status fcm_fcoe_if_action(char *, char *);

struct fcm_clif {
	int cl_fd;
	int cl_busy;		/* non-zero if command pending */
	int cl_ping_pending;
	struct sockaddr_un cl_local;
};

static struct fcm_clif fcm_clif_st;
static struct fcm_clif *fcm_clif = &fcm_clif_st;
static struct sa_timer fcm_dcbd_timer;

/* Debugging routine */
static void print_errors(int errors);

struct fcm_netif_head fcm_netif_head = TAILQ_HEAD_INITIALIZER(fcm_netif_head);

static int fcm_fc_socket;

static int fcm_link_socket;
static int fcm_link_seq;
static void fcm_link_recv(void *);
static void fcm_link_getlink(void);
static int fcm_link_buf_check(size_t);
static void clear_dcbd_info(struct fcm_netif *ff);

/*
 * Table for getopt_long(3).
 */
static struct option fcm_options[] = {
	{"debug", 0, NULL, 'd'},
	{"syslog", 0, NULL, 's'},
	{"exec", 1, NULL, 'e'},
	{"foreground", 0, NULL, 'f'},
	{"version", 0, NULL, 'v'},
	{NULL, 0, NULL, 0}
};

char progname[20];

static char fcm_pidfile[] = CLIF_PID_FILE;

/*
 * Issue with buffer size:  It isn't clear how to read more than one
 * buffer's worth of GETLINK replies.  The kernel seems to just drop the
 * interface messages if they don't fit in the buffer, so we just make it
 * large enough to fit and expand it if we ever do a read that almost fills it.
 */
static char *fcm_link_buf;
static size_t fcm_link_buf_size = 4096;	/* initial size */
static const size_t fcm_link_buf_fuzz = 300;	/* "almost full" remainder */

/*
 * A value must be surrounded by quates, e.g. "x".
 * The minimum length of a value is 1 excluding the quotes.
 * The maximum length of a value is 20 excluding the quotes.
 */
static int fcm_remove_quotes(char *buf, int len)
{
	char *s = buf;
	char *e = buf + len - 1;
	char tmp[CONFIG_MAX_VAL_LEN + 1];

	if (len < CONFIG_MIN_VAL_LEN)
		return -1;
	if ((*s >= '0' && *s <= '9') ||
	    (*s >= 'a' && *s <= 'z') ||
	    (*s >= 'A' && *s <= 'Z'))
		return -1;
	if ((*e >= '0' && *e <= '9') ||
	    (*e >= 'a' && *e <= 'z') ||
	    (*e >= 'A' && *e <= 'Z'))
		return -1;
	s = buf + 1;
	*e = '\0';
	strncpy(tmp, s, len - 1);
	strncpy(buf, tmp, len - 1);

	return 0;
}

/*
 * Read a configuration variable for a port from a config file.
 * There's no problem if the file doesn't exist.
 * The buffer is set to an empty string if the variable is not found.
 *
 * Returns:  1    found
 *           0    not found
 *           -1   error in format
 */
static size_t fcm_read_config_variable(char *file, char *val_buf, size_t len,
				       FILE *fp, const char *var_name)
{
	char *s;
	char *var;
	char *val;
	char buf[FILE_NAME_LEN];
	int n;

	val_buf[0] = '\0';
	buf[sizeof(buf) - 1] = '\0';
	rewind(fp);
	while ((s = fgets(buf, sizeof(buf) - 1, fp)) != NULL) {
		while (isspace(*s))
			s++;
		if (*s == '\0' || *s == '#')
			continue;
		var = s;
		if (!isalpha(*var))
			continue;
		val = strchr(s, '=');
		if (val == NULL)
			continue;
		*val++ = '\0';
		s = val;
		if (strcmp(var_name, var) != 0)
			continue;
		while (*s != '\0' && !isspace(*s))
			s++;
		*s = '\0';
		n = snprintf(val_buf, len, "%s", val);
		if (fcm_remove_quotes(val_buf, n) < 0) {
			FCM_LOG("Invalid format in config file"
				" %s: %s=%s\n",
				file, var_name, val);
			/* error */
			return -1;
		}
		/* found */
		FCM_LOG_DBG("%s: %s = %s\n", file, var_name, val);
		return 1;
	}
	/* not found */
	return 0;
}

static struct fcoe_port *alloc_fcoe_port(char *ifname)
{
	struct fcoe_port *p = NULL;

	p = (struct fcoe_port *) calloc(1, sizeof(struct fcoe_port));
	if (p) {
		snprintf(p->ifname, sizeof(p->ifname), "%s", ifname);
		p->action = FCP_WAIT;
		/* last_action is initialized to FCP_DESTROY_IF to indicate
		 * that the interface is not created yet.
		 */
		p->last_action = FCP_DESTROY_IF;
		p->fip_socket = -1;
		p->fchost[0] = '\0';
		p->last_fc_event_num = 0;
		sa_timer_init(&p->vlan_disc_timer, fcm_vlan_disc_timeout, p);
	}

	return p;
}

static int fcm_read_config_files(void)
{
	char file[80];
	FILE *fp;
	char val[CONFIG_MAX_VAL_LEN + 1];
	DIR *dir;
	struct dirent *dp;
	struct fcoe_port *curr = NULL;
	struct fcoe_port *next = NULL;
	int rc;

	dir = opendir(CONFIG_DIR);
	if (dir == NULL) {
		FCM_LOG_ERR(errno, "Failed reading directory %s\n", CONFIG_DIR);
		return -1;
	}
	for (;;) {
		dp = readdir(dir);
		if (dp == NULL)
			break;
		if (dp->d_name[0] == '.' &&
		    (dp->d_name[1] == '\0' ||
		     (dp->d_name[1] == '.' && dp->d_name[2] == '\0')))
			continue;
		rc = strncmp(dp->d_name, CFG_FILE_PREFIX,
			     strlen(CFG_FILE_PREFIX));
		if (rc)
			continue;

		if (!strncmp(dp->d_name, DEF_CFG_FILE,
			     strlen(DEF_CFG_FILE)))
			continue;

		next = alloc_fcoe_port(dp->d_name + 4);

		if (!next) {
			FCM_LOG_ERR(errno, "failed to allocate fcoe_port %s",
				    dp->d_name);
			continue;
		}
		strncpy(file, CONFIG_DIR "/", sizeof(file));
		strncat(file, dp->d_name, sizeof(file) - strlen(file));
		fp = fopen(file, "r");
		if (!fp) {
			FCM_LOG_ERR(errno, "Failed to read %s\n", file);
			free(next);
			continue;
		}

		/* FCOE_ENABLE */
		rc = fcm_read_config_variable(file, val, sizeof(val),
					      fp, "FCOE_ENABLE");
		if (rc < 0) {
			FCM_LOG("%s invalid format for FCOE_ENABLE setting");
			fclose(fp);
			free(next);
			continue;
		}
		/* if not found, default to "no" */
		if (!strncasecmp(val, "yes", 3) && rc == 1)
			next->fcoe_enable = 1;

		/* DCB_REQUIRED */
		rc = fcm_read_config_variable(file, val, sizeof(val),
					      fp, "DCB_REQUIRED");
		if (rc < 0) {
			FCM_LOG("%s invalid format for DCB_REQUIRED setting");
			fclose(fp);
			free(next);
			continue;
		}
		/* if not found, default to "no" */
		if (!strncasecmp(val, "yes", 3) && rc == 1)
			next->dcb_required = 1;

		/* AUTO_VLAN */
		rc = fcm_read_config_variable(file, val, sizeof(val),
					      fp, "AUTO_VLAN");
		if (rc < 0) {
			FCM_LOG("%s invalid format for AUTO_VLAN setting");
			fclose(fp);
			free(next);
			continue;
		}
		/* if not found, default to "no" */
		if (!strncasecmp(val, "yes", 3) && rc == 1)
			next->auto_vlan = 1;

		fclose(fp);

		if (!fcoe_config.port) {
			fcoe_config.port = next;
			curr = next;
		} else {
			curr->next = next;
			curr = next;
		}
	}
	closedir(dir);
	return 0;
}

/*
 * Given an fcoe_port pointer and an ifname, find the next fcoe_port
 * in the list with a real ifname of 'ifname'.
 *
 * Returns:  fcoe_port pointer to fcoe port entry
 *           NULL - if not found
 */
static struct fcoe_port *fcm_find_next_fcoe_port(struct fcoe_port *p,
						 char *ifname)
{
	struct fcoe_port *np;

	np = fcoe_config.port;
	while (np) {
		if (np == p)
			break;
		np = np->next;
	}

	if (np)
		np = np->next;

	while (np) {
		if (!strncmp(ifname, np->real_ifname, IFNAMSIZ))
			return np;
		np = np->next;
	}

	return NULL;
}

static struct fcoe_port *fcm_find_fcoe_port(char *ifname,
					    enum fcoeport_ifname t)
{
	struct fcoe_port *p;
	char *fp_ifname;

	p = fcoe_config.port;
	while (p) {
		switch (t) {
		case FCP_CFG_IFNAME:
			fp_ifname = p->ifname;
			break;
		case FCP_REAL_IFNAME:
			fp_ifname = p->real_ifname;
			break;
		default:
			FCM_LOG("unhandled interface type [%d] for %s",
				t, ifname);
			return NULL;
		}

		if (!strncmp(ifname, fp_ifname, IFNAMSIZ))
			return p;
		p = p->next;
	}
	return NULL;
}

static struct fcoe_port *fcm_find_port_by_host(uint16_t host_no)
{
	struct fcoe_port *p;
	char host[FCHOSTBUFLEN];

	snprintf(host, FCHOSTBUFLEN, "host%d", host_no);
	p = fcoe_config.port;
	while (p) {
		if (!strncmp(p->fchost, host, FCHOSTBUFLEN))
			return p;
		p = p->next;
	}
	return NULL;
}

static void fcm_fc_event_handler(struct fc_nl_event *fc_event)
{
	struct fcoe_port *p = fcm_find_port_by_host(fc_event->host_no);

	if (!p)
		return;

	switch (fc_event->event_code) {
	case HBA_EVENT_LIP_RESET_OCCURRED:
		if (!p->last_fc_event_num &&
		    fc_event->event_num == p->last_fc_event_num)
			return;

		if (!p->auto_created && !p->auto_vlan)
			return;

		p->last_fc_event_num = fc_event->event_num;

		/* find real interface port and re-activate again */
		p = fcm_find_fcoe_port(p->real_ifname, FCP_CFG_IFNAME);
		if (p)
			fcp_set_next_action(p, FCP_ACTIVATE_IF);
		break;
	default:
		FCM_LOG("unsupported fc event:%d for host:%d\n",
			fc_event->event_code, fc_event->host_no);
	}
}

static void fcm_fc_event_recv(void *arg)
{
	struct nlmsghdr *hp;
	struct fc_nl_event *fc_event;
	int plen;
	int rlen;
	char *buf;

	buf = malloc(DEF_RX_BUF_SIZE);

	if (!buf) {
		FCM_LOG_ERR(errno, "failed to allocate FC event buffer\n");
		return;
	}

	rlen = read(fcm_fc_socket, buf, DEF_RX_BUF_SIZE);
	if (!rlen)
		goto free_buf;

	if (rlen < 0) {
		FCM_LOG_ERR(errno, "fc read error");
		goto free_buf;
	}

	hp = (struct nlmsghdr *)buf;
	for (hp = (struct nlmsghdr *)buf; NLMSG_OK(hp, rlen);
	     hp = NLMSG_NEXT(hp, rlen)) {

		FCM_LOG("received fc event message %d\n", __LINE__);
		if (hp->nlmsg_type == NLMSG_DONE)
			break;

		if (hp->nlmsg_type == NLMSG_ERROR) {
			FCM_LOG("fc nlmsg error");
			break;
		}

		plen = NLMSG_PAYLOAD(hp, 0);
		fc_event = (struct fc_nl_event *)NLMSG_DATA(hp);
		if (plen < sizeof(*fc_event)) {
			FCM_LOG("too short (%d) to be an FC event", rlen);
			break;
		}
		FCM_LOG("seconds:%ld host%d event_datalen:%d\n",
			fc_event->seconds, fc_event->host_no,
			fc_event->event_datalen);
		FCM_LOG("event_num:%d event_code:%d event_data:%d\n",
			fc_event->event_num, fc_event->event_code,
			fc_event->event_data);
		fcm_fc_event_handler(fc_event);
	}
free_buf:
	free(buf);
}

static int fcm_fc_events_init(void)
{
	int fd, rc;
	struct sockaddr_nl fc_local;

	fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_SCSITRANSPORT);
	if (fd < 0) {
		FCM_LOG_ERR(errno, "fc socket error");
		return fd;
	}
	memset(&fc_local, 0, sizeof(fc_local));
	fc_local.nl_family = AF_NETLINK;
	fc_local.nl_groups = ~0;
	fc_local.nl_pid = getpid();
	rc = bind(fd, (struct sockaddr *)&fc_local, sizeof(fc_local));
	if (rc == -1) {
		FCM_LOG_ERR(errno, "fc socket bind error");
		close(fd);
		return rc;
	}
	fcm_fc_socket = fd;

	/* Add a given file descriptor readfds set with its rx handler */
	sa_select_add_fd(fd, fcm_fc_event_recv, NULL, NULL, NULL);
	return 0;
}

static int fcm_link_init(void)
{
	int fd;
	int rc;
	struct sockaddr_nl l_local;

	fcm_link_buf = malloc(fcm_link_buf_size);
	ASSERT(fcm_link_buf);

	fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (fd < 0) {
		FCM_LOG_ERR(errno, "socket error");
		return fd;
	}
	memset(&l_local, 0, sizeof(l_local));
	l_local.nl_family = AF_NETLINK;
	l_local.nl_groups = RTMGRP_LINK | (1 << (RTNLGRP_DCB - 1));
	l_local.nl_pid = 0;
	rc = bind(fd, (struct sockaddr *)&l_local, sizeof(l_local));
	if (rc == -1) {
		FCM_LOG_ERR(errno, "bind error");
		close(fd);
		return rc;
	}
	fcm_link_socket = fd;

	/* Add a given file descriptor from a readfds set */
	sa_select_add_fd(fd, fcm_link_recv, NULL, NULL, NULL);

	fcm_link_getlink();

	return 0;
}

static struct fcoe_port *fcm_port_create(char *ifname, int cmd);

struct fcoe_port *fcm_new_vlan(int ifindex, int vid)
{
	char real_name[IFNAMSIZ];
	char vlan_name[IFNAMSIZ];
	struct fcoe_port *p;

	FCM_LOG_DBG("Auto VLAN Found FCF on VID %d\n", vid);

	if (rtnl_find_vlan(ifindex, vid, vlan_name)) {
		rtnl_get_linkname(ifindex, real_name);
		snprintf(vlan_name, IFNAMSIZ, "%s.%d-fcoe", real_name, vid);
		vlan_create(ifindex, vid, vlan_name);
	}
	rtnl_set_iff_up(0, vlan_name);
	p = fcm_find_fcoe_port(vlan_name, FCP_CFG_IFNAME);
	if (p && !p->fcoe_enable)
		return p;
	p = fcm_port_create(vlan_name, FCP_ACTIVATE_IF);
	p->auto_created = 1;
	return p;
}


int fcm_vlan_disc_handler(struct fiphdr *fh, struct sockaddr_ll *sa, void *arg)
{
	int vid;
	unsigned char mac[ETHER_ADDR_LEN];
	int len = ntohs(fh->fip_desc_len);
	struct fip_tlv_hdr *tlv = (struct fip_tlv_hdr *)(fh + 1);
	struct fcoe_port *p = arg;
	struct fcoe_port *vp;
	int desc_mask = 0;

	enum {
		VALID_MAC	= 1,
		VALID_VLAN	= 2,
	};

	if (ntohs(fh->fip_proto) != FIP_PROTO_VLAN)
		return -1;

	if (fh->fip_subcode != FIP_VLAN_NOTE)
		return -1;

	while (len > 0) {
		switch (tlv->tlv_type) {
		case FIP_TLV_MAC_ADDR:
			memcpy(mac, ((struct fip_tlv_mac_addr *)tlv)->mac_addr,
			       ETHER_ADDR_LEN);
			desc_mask |= VALID_MAC;
			break;
			/*
			 * this expects to see the MAC_ADDR TLV first,
			 * and is broken if not
			 */
		case FIP_TLV_VLAN:
			if (tlv->tlv_len != 1) {
				FCM_LOG_ERR(EINVAL, "bad length on VLAN TLV");
				break;
			}
			vid = ntohs(((struct fip_tlv_vlan *)tlv)->vlan);
			vp = fcm_new_vlan(sa->sll_ifindex, vid);
			vp->dcb_required = p->dcb_required;
			desc_mask |= VALID_VLAN;
			break;
		default:
			/* unexpected or unrecognized descriptor */
			FCM_LOG_DBG("ignoring TLV type %d", tlv->tlv_type);
			break;
		}
		len -= tlv->tlv_len;
		tlv = ((void *) tlv) + (tlv->tlv_len << 2);
	};

	if (desc_mask == (VALID_MAC | VALID_VLAN)) {
		/* cancel the retry timer, valid response received */
		sa_timer_cancel(&p->vlan_disc_timer);
		return 0;
	} else {
		return -1;
	}
}

static void fcm_fip_recv(void *arg)
{
	struct fcoe_port *p = arg;
	fip_recv(p->fip_socket, fcm_vlan_disc_handler, p);
}

static int fcm_vlan_disc_socket(struct fcoe_port *p)
{
	int fd;
	int origdev = 1;

	fd = fip_socket(p->ifindex);
	if (fd < 0) {
		FCM_LOG_ERR(errno, "socket error");
		return fd;
	}
	setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &origdev, sizeof(origdev));
	sa_select_add_fd(fd, fcm_fip_recv, NULL, NULL, p);
	return fd;
}


/* fcm_vlan_dev_real_dev - query vlan real_dev
 * @vlan_ifname - vlan device ifname to find real interface name for
 * @real_ifname - pointer to copy real ifname to
 *
 * Make an ioctl call to find the real device for vlan_ifname.
 * Copy to real_ifname if found.
 */
static void fcm_vlan_dev_real_dev(char *vlan_ifname, char *real_ifname)
{
	int fd;
	struct vlan_ioctl_args ifv;

	real_ifname[0] = '\0';

	fd = socket(PF_INET, SOCK_DGRAM, 0);

	if (fd < 0) {
		FCM_LOG_ERR(errno, "open vlan query socket error");
		return;
	}

	memset(&ifv, 0, sizeof(ifv));
	ifv.cmd = GET_VLAN_REALDEV_NAME_CMD;
	strncpy(ifv.device1, vlan_ifname, strlen(vlan_ifname)+1);
	if (ioctl(fd, SIOCGIFVLAN, &ifv) == 0)
		strncpy(real_ifname, ifv.u.device2, strlen(ifv.u.device2)+1);
	close(fd);
}

/* fcm_is_linkinfo_vlan - parse nlmsg linkinfo rtattr for vlan kind
 * @ap: pointer to the linkinfo rtattr
 *
 * This function parses the linkinfo rtattr and returns
 * 1 if it is kind vlan otherwise returns 0.
 */
int fcm_is_linkinfo_vlan(struct rtattr *ap)
{
	struct rtattr *info;
	int len;

	info = (struct rtattr *) (RTA_DATA(ap));

	for (len = ap->rta_len; RTA_OK(info, len); info = RTA_NEXT(info, len)) {
		if (info->rta_type != IFLA_INFO_KIND)
			continue;

		if (strncmp("vlan", RTA_DATA(info), sizeof("vlan")))
			return 0;
		else
			return 1;
	}

	return 0;
}


/* fcm_set_next_action - determine the next action for the FCoE interface
 * @p - pointer to the fcoe_port structure for the FCoE interface
 * @action - requested next action to take on the FCoE interface
 *
 * Based on the last_action taken on the FCoE interface and the requested
 * next action, the next action field in the FCoE interface's fcoe_port
 * structure is set.
 * Notes: last_action is initialized to FCP_DESTROY_IF when the fcoe_port is
 *        created and it is never set to FCP_WAIT.
 *        The requested action FCP_ACTIVATE_IF is resolved to either
 *        FCP_CREATE_IF or FCP_ENABLE_IF as appropriate.
 */
static void fcp_set_next_action(struct fcoe_port *p, enum fcp_action action)
{
	switch (p->last_action) {
	case FCP_CREATE_IF:
		switch (action) {
		case FCP_DESTROY_IF:
		case FCP_ENABLE_IF:
		case FCP_DISABLE_IF:
		case FCP_RESET_IF:
		case FCP_SCAN_IF:
			p->action = action;
			break;
		case FCP_ACTIVATE_IF:
			if (p->auto_vlan)
				p->action = FCP_VLAN_DISC;
			else
				p->action = FCP_ENABLE_IF;
			break;
		default:
			p->action = FCP_WAIT;
			break;
		}
		break;
	case FCP_DESTROY_IF:
		switch (action) {
		case FCP_CREATE_IF:
		case FCP_ACTIVATE_IF:
			if (p->auto_vlan)
				p->action = FCP_VLAN_DISC;
			else
				p->action = FCP_CREATE_IF;
			break;
		default:
			p->action = FCP_WAIT;
			break;
		}
		break;
	case FCP_ENABLE_IF:
		switch (action) {
		case FCP_DESTROY_IF:
		case FCP_DISABLE_IF:
		case FCP_RESET_IF:
		case FCP_SCAN_IF:
			p->action = action;
			break;
		default:
			p->action = FCP_WAIT;
			break;
		}
		break;
	case FCP_DISABLE_IF:
		switch (action) {
		case FCP_DESTROY_IF:
		case FCP_RESET_IF:
			p->action = action;
			break;
		case FCP_ENABLE_IF:
		case FCP_ACTIVATE_IF:
			if (p->auto_vlan)
				p->action = FCP_VLAN_DISC;
			else
				p->action = FCP_ENABLE_IF;
			break;
		default:
			p->action = FCP_WAIT;
			break;
		}
		break;
	case FCP_RESET_IF:
	case FCP_SCAN_IF:
		switch (action) {
		case FCP_DESTROY_IF:
		case FCP_DISABLE_IF:
		case FCP_RESET_IF:
		case FCP_SCAN_IF:
			p->action = action;
			break;
		case FCP_ENABLE_IF:
		case FCP_ACTIVATE_IF:
			if (p->auto_vlan)
				p->action = FCP_VLAN_DISC;
			else
				p->action = FCP_ENABLE_IF;
			break;
		default:
			p->action = FCP_WAIT;
			break;
		}
		break;
	case FCP_VLAN_DISC:
		switch (action) {
		case FCP_ACTIVATE_IF:
			if (p->auto_vlan)
				p->action = FCP_VLAN_DISC;
			else
				p->action = FCP_CREATE_IF;
			break;
		case FCP_DESTROY_IF:
		case FCP_DISABLE_IF:
		case FCP_RESET_IF:
		case FCP_SCAN_IF:
			if (p->fip_socket >= 0) {
				sa_timer_cancel(&p->vlan_disc_timer);
				sa_select_rem_fd(p->fip_socket);
				close(p->fip_socket);
				p->fip_socket = -1;
			}
			p->action = action;
			break;
		default:
			p->action = FCP_WAIT;
			break;
		}
		break;
	default:
		/* last_action is never set to FCP_WAIT */
		break;
	}
}

static void fcp_action_set(char *ifname, enum fcp_action action)
{
	struct fcoe_port *p;

	p = fcm_find_fcoe_port(ifname, FCP_REAL_IFNAME);
	while (p) {
		if (p->fcoe_enable) {
			switch (action) {
			case FCP_ACTIVATE_IF:
				/*
				 * let the VLAN discovery code
				 * enabled auto-VLANs
				 */
				if (!p->auto_created)
					fcp_set_next_action(p, FCP_ACTIVATE_IF);
				else
					fcp_set_next_action(p, FCP_WAIT);
				break;
			default:
				fcp_set_next_action(p, action);
			}
		}
		p = fcm_find_next_fcoe_port(p, ifname);
	}
}

/*
 * Send DCB_CMD_IEEE_GET request for an interface.
 */
static void ieee_get_req(struct fcm_netif *ff)
{
	int iflen;
	int rc;
	int seq;
	struct {
		struct nlmsghdr nl;
		struct dcbmsg dcbmsg;
		struct rtattr rta;
		char ifname[IFNAMSIZ];
	} msg;

	seq = ++fcm_link_seq;
	if (!seq)
		seq = ++fcm_link_seq;

	iflen = strlen(ff->ifname);
	if (iflen >= IFNAMSIZ)
		iflen = IFNAMSIZ - 1;

	memset(&msg, 0, sizeof(msg));
	msg.nl.nlmsg_len = NLMSG_ALIGN(sizeof(msg) - sizeof(msg.ifname) +
				iflen + 1);
	msg.nl.nlmsg_type = RTM_GETDCB;
	msg.nl.nlmsg_flags = NLM_F_REQUEST;
	msg.nl.nlmsg_seq = seq;
	msg.nl.nlmsg_pid = getpid();
	msg.dcbmsg.cmd = DCB_CMD_IEEE_GET;
	msg.dcbmsg.dcb_family = AF_UNSPEC;
	msg.dcbmsg.dcb_pad = 0;
	msg.rta.rta_len = NLMSG_ALIGN(NLA_HDRLEN + iflen + 1);
	msg.rta.rta_type = DCB_ATTR_IFNAME;
	strncpy(msg.ifname, ff->ifname, iflen);
	ff->ieee_resp_pending = seq;
	rc = write(fcm_link_socket, &msg, msg.nl.nlmsg_len);
	if (rc < 0) {
		printf("%s: %s: write failed\n", __func__, ff->ifname);
		ff->ieee_resp_pending = 0;
	}
}

/*
 * clear_ieee_info - Clear IEEE info to unknown values
 */
static void clear_ieee_info(struct fcm_netif *ff)
{
	ff->ieee_pfc_info = 0;
	ff->ieee_app_info = 0;
	ff->dcbx_cap = 0;
}

STR_ARR(ieee_states, "Unknown", "Out of range",
	[IEEE_INIT] = "IEEE_INIT",
	[IEEE_GET_STATE] = "IEEE_GET_STATE",
	[IEEE_DONE] = "IEEE_DONE",
);

static void
ieee_state_set(struct fcm_netif *ff, enum ieee_state new_state)
{
	if (ff->ff_operstate != IF_OPER_UP) {
		ff->ieee_state = IEEE_INIT;
		return;
	}

	if (fcoe_config.debug) {
		FCM_LOG_DEV_DBG(ff, "%s -> %s",
				getstr(&ieee_states, ff->ieee_state),
				getstr(&ieee_states, new_state));
	}

	if (new_state == IEEE_GET_STATE) {
		ff->ieee_state = new_state;
		clear_ieee_info(ff);
		ieee_get_req(ff);
		return;
	}

	ff->ieee_state = new_state;
	ff->ieee_resp_pending = 0;
}

static struct sa_nameval fcm_dcbd_states[] = FCM_DCBD_STATES;

static void fcm_dcbd_state_set(struct fcm_netif *ff,
			       enum fcm_dcbd_state new_state)
{
	if (ff->ff_operstate != IF_OPER_UP) {
		ff->ff_dcbd_state = FCD_INIT;
		return;
	}

	if (fcoe_config.debug) {
		char old[32];
		char new[32];

		FCM_LOG_DEV_DBG(ff, "%s -> %s",
				sa_enum_decode(old, sizeof(old),
					       fcm_dcbd_states,
					       ff->ff_dcbd_state),
				sa_enum_decode(new, sizeof(new),
					       fcm_dcbd_states, new_state));
	}

	if (new_state == FCD_GET_DCB_STATE)
		clear_dcbd_info(ff);

	if (new_state == FCD_INIT) {
		ff->dcbd_retry_cnt = 0;
		sa_timer_cancel(&ff->dcbd_retry_timer);
	}

	if (new_state == FCD_ERROR) {
		ff->dcbd_retry_cnt++;
		FCM_LOG_DEV_DBG(ff, "%s: SETTING lldpad RETRY TIMER  = %d\n",
				ff->ifname,
				ff->dcbd_retry_cnt * DCBD_REQ_RETRY_TIMEOUT);
		sa_timer_set(&ff->dcbd_retry_timer,
			     ff->dcbd_retry_cnt * DCBD_REQ_RETRY_TIMEOUT);
	}

	ff->ff_dcbd_state = new_state;
	ff->response_pending = 0;
}

static void update_fcoe_port_state(struct fcoe_port *p, unsigned int type,
				   u_int8_t operstate, enum fcoeport_ifname t)
{
	struct fcm_netif *ff = NULL;

	if (type != RTM_DELLINK) {
		ff = fcm_netif_lookup_create(p->real_ifname);
		if (!ff)
			return;

		/* Only set the ff_operstate field of the network interface
		 * element if this routine is being called for the real
		 * network interface, or, if the interface is a VLAN, if the
		 * network interface element has not been intialized and the
		 * VLAN operstate is up (if VLAN is up, then real interface is
		 * up).
		 */
		if ((t == FCP_REAL_IFNAME) ||
		    ((t == FCP_CFG_IFNAME) &&
		     (ff->ff_operstate == IF_OPER_UNKNOWN) &&
		     (operstate == IF_OPER_UP)))
			ff->ff_operstate = operstate;

		if (!p->fcoe_enable) {
			fcp_set_next_action(p, FCP_DESTROY_IF);
			return;
		}

		if (operstate == IF_OPER_UP) {
			if (p->dcb_required) {
				/* If DCB is required, do not start the dcbd
				 * query sequence if this routine is being
				 * called for a real interface and the FCoE
				 * interface is configured on a VLAN.
				 */
				if (!((t == FCP_REAL_IFNAME) &&
				      strncmp(p->ifname, p->real_ifname,
					      IFNAMSIZ))) {
					fcm_dcbd_state_set(ff,
							   FCD_GET_DCB_STATE);
					ieee_state_set(ff, IEEE_GET_STATE);
				}
			} else {
				/* hold off on auto-created VLAN ports until
				 * VLAN discovery can validate that the setup
				 * has not changed */
				if (!p->auto_created || !p->auto_vlan)
					fcp_set_next_action(p, FCP_ACTIVATE_IF);
			}
		} else {
			fcp_set_next_action(p, FCP_DISABLE_IF);
		}
	} else {
		fcp_set_next_action(p, FCP_DESTROY_IF);
	}
}

void fcm_process_link_msg(struct ifinfomsg *ip, int len, unsigned type)
{
	struct fcoe_port *p;
	struct rtattr *ap;
	char ifname[IFNAMSIZ];
	char real_dev[IFNAMSIZ];
	u_int8_t operstate;
	unsigned char mac[ETHER_ADDR_LEN];
	int is_vlan;
	int ifindex;

	is_vlan = 0;
	operstate = IF_OPER_UNKNOWN;

	ifindex = ip->ifi_index;

	if (ip->ifi_type != ARPHRD_ETHER)
		return;

	len -= sizeof(*ip);
	for (ap = (struct rtattr *)(ip + 1); RTA_OK(ap, len);
	     ap = RTA_NEXT(ap, len)) {
		switch (ap->rta_type) {
		case IFLA_ADDRESS:
			if (RTA_PAYLOAD(ap) == 6)
				memcpy(mac, RTA_DATA(ap), ETHER_ADDR_LEN);
			break;

		case IFLA_IFNAME:
			sa_strncpy_safe(ifname, sizeof(ifname),
					RTA_DATA(ap),
					RTA_PAYLOAD(ap));
			FCM_LOG_DBG("ifname %s", ifname);
			break;

		case IFLA_OPERSTATE:
			operstate = *(uint8_t *) RTA_DATA(ap);
			break;

		case IFLA_LINKINFO:
			if (fcm_is_linkinfo_vlan(ap))
				is_vlan = 1;
			break;

		default:
			break;
		}
	}

	p = fcm_find_fcoe_port(ifname, FCP_CFG_IFNAME);
	if (is_vlan) {
		/* if not in fcoe port list, then ignore this ifname */
		if (!p)
			return;

		p->ifindex = ifindex;
		memcpy(p->mac, mac, ETHER_ADDR_LEN);

		/* don't do VLAN discovery on a VLAN */
		p->auto_vlan = 0;

		/* try to find the real device name */
		real_dev[0] = '\0';
		fcm_vlan_dev_real_dev(ifname, real_dev);
		if (strlen(real_dev))
			strncpy(p->real_ifname, real_dev, strlen(real_dev)+1);
		update_fcoe_port_state(p, type, operstate, FCP_CFG_IFNAME);
		p->last_msg_type = type;
	} else {
		/* the ifname is not a VLAN.  handle the case where it has
		 * an FCoE interface configured on it.
		 */
		if (p) {
			p->ifindex = ifindex;
			memcpy(p->mac, mac, ETHER_ADDR_LEN);
			strncpy(p->real_ifname, ifname, strlen(ifname)+1);
			update_fcoe_port_state(p, type, operstate,
					       FCP_REAL_IFNAME);
		}

		/* handle all FCoE ports which are on VLANs over this
		 * ifname.
		 */
		p = fcm_find_fcoe_port(ifname, FCP_REAL_IFNAME);
		while (p) {
			update_fcoe_port_state(p, type, operstate,
					       FCP_REAL_IFNAME);
			p = fcm_find_next_fcoe_port(p, ifname);
		}
	}
}

static struct rtattr *find_nested_attr(struct rtattr *rta, __u16 type)
{
	struct rtattr *rta_child;

	rta_child = NLA_DATA(rta);
	rta = NLA_NEXT(rta);

	for (; rta > rta_child; rta_child = NLA_NEXT(rta_child))
		if (rta_child->rta_type == type)
			return rta_child;

	return NULL;
}

static struct rtattr *find_attr(struct nlmsghdr *nlh, __u16 type)
{
	struct rtattr *rta;
	int len;

	rta = (struct rtattr *)(((char *)NLMSG_DATA(nlh)) +
		NLMSG_ALIGN(sizeof(struct dcbmsg)));
	len = NLMSG_PAYLOAD(nlh, 0) - sizeof(struct dcbmsg);

	while (RTA_OK(rta, len)) {
		if (rta->rta_type == type)
			return rta;

		rta = RTA_NEXT(rta, len);
	}

	return NULL;
}

static int ieee_get_dcbx(struct nlmsghdr *nlh)
{
	struct rtattr *rta;

	rta = find_attr(nlh, DCB_ATTR_DCBX);
	if (!rta)
		return -EIO;

	return *(__u8 *)NLA_DATA(rta);
}

static int get_pri_mask_from_ieee(struct rtattr *rta, __u8 dcbx_cap)
{
	struct rtattr *rta_parent;
	struct rtattr *rta_child;
	int rval;
	__u8 ieee = dcbx_cap & DCB_CAP_DCBX_VER_IEEE;

	rta_parent = find_nested_attr(rta, DCB_ATTR_IEEE_APP_TABLE);
	if (!rta_parent)
		return -EIO;

	rta_child = NLA_DATA(rta_parent);
	rta_parent = NLA_NEXT(rta_parent);

	rval = 0;
	for (; rta_parent > rta_child; rta_child = NLA_NEXT(rta_child)) {
		struct dcb_app *app;

		if (rta_child->rta_type != DCB_ATTR_IEEE_APP)
			continue;

		app = (struct dcb_app *)NLA_DATA(rta_child);
		if (app->protocol != FCOE_ETH_TYPE)
			continue;

		if (ieee) {
			if (app->selector == IEEE_8021QAZ_APP_SEL_ETHERTYPE)
				rval |= 1 << app->priority;
		} else {
			if (app->selector == DCB_APP_IDTYPE_ETHTYPE)
				return app->priority;
		}
	}

	return rval;
}

static void fcm_process_ieee_msg(struct nlmsghdr *nlh)
{
	struct dcbmsg *d;
	struct rtattr *rta_parent;
	struct rtattr *rta_child;
	struct fcm_netif *ff;
	int dcbx_cap;
	int pri_mask;
	char ifname[IFNAMSIZ];

	d = (struct dcbmsg *)NLMSG_DATA(nlh);
	if (d->cmd != DCB_CMD_IEEE_GET && d->cmd != DCB_CMD_IEEE_SET) {
		FCM_LOG("%s: Unexpected command type %d\n", __func__, d->cmd);
		return;
	}

	rta_parent = (struct rtattr *)(((char *)d) + NLMSG_ALIGN(sizeof(*d)));
	if (rta_parent->rta_type != DCB_ATTR_IFNAME) {
		FCM_LOG("%s: ifname not found\n", __func__);
		return;
	}

	strncpy(ifname, NLA_DATA(rta_parent), sizeof(ifname));
	ff = fcm_netif_lookup_create(ifname);
	if (!ff) {
		FCM_LOG("%s: if %s not found or created\n", __func__, ifname);
		return;
	}

	dcbx_cap = ieee_get_dcbx(nlh);
	if (dcbx_cap < 0) {
		FCM_LOG("%s: %s: ieee_get_dcbx returned %d\n", __func__,
			ifname, dcbx_cap);
		return;
	}
	ff->dcbx_cap = dcbx_cap;
	if (!ff->ff_dcb_state)
		ff->ff_dcb_state = !!(dcbx_cap & DCB_CAP_DCBX_VER_IEEE);
	if (d->cmd == DCB_CMD_IEEE_SET && !(dcbx_cap & DCB_CAP_DCBX_VER_IEEE)) {
		FCM_LOG("%s: %s: IEEE msg while not in IEEE mode\n", __func__,
			ifname);
	}

	rta_parent = find_attr(nlh, DCB_ATTR_IEEE);
	if (!rta_parent) {
		FCM_LOG("%s: %s: No IEEE attr found\n", __func__, ifname);
		return;
	}

	rta_child = find_nested_attr(rta_parent, DCB_ATTR_IEEE_PFC);
	if (!rta_child) {
		FCM_LOG("%s: %s: IEEE PFC attr not found\n", __func__, ifname);
		return;
	}

	struct ieee_pfc *ieee_pfc = (struct ieee_pfc *)NLA_DATA(rta_child);

	ff->ieee_pfc_info = ieee_pfc->pfc_en;

	pri_mask = get_pri_mask_from_ieee(rta_parent, dcbx_cap);
	if (pri_mask < 0) {
		FCM_LOG("%s: %s: Error getting pri from IEEE attr\n", __func__,
			ifname);
		return;
	}
	FCM_LOG_DBG("%s: %s: FCoE pri mask = 0x%02X\n", __func__,
		    ifname, pri_mask);
	ff->ieee_app_info = pri_mask;

	if (ff->ieee_state == IEEE_GET_STATE && d->cmd == DCB_CMD_IEEE_GET &&
	    ff->ieee_resp_pending == nlh->nlmsg_seq)
		ieee_state_set(ff, IEEE_DONE);
}

static void fcm_link_recv(void *arg)
{
	int rc;
	char *buf;
	struct nlmsghdr *hp;
	struct ifinfomsg *ip;
	unsigned type;
	int plen;
	int rlen;

	buf = fcm_link_buf;
	rc = read(fcm_link_socket, buf, fcm_link_buf_size);
	if (rc <= 0) {
		if (rc < 0)
			FCM_LOG_ERR(errno, "read error");
		return;
	}

	if (fcm_link_buf_check(rc)) {
		fcm_link_getlink();
		return;
	}

	hp = (struct nlmsghdr *)buf;
	rlen = rc;
	for (hp = (struct nlmsghdr *)buf; NLMSG_OK(hp, rlen);
	     hp = NLMSG_NEXT(hp, rlen)) {

		type = hp->nlmsg_type;
		if (hp->nlmsg_type == NLMSG_DONE)
			break;

		if (hp->nlmsg_type == NLMSG_ERROR) {
			FCM_LOG("nlmsg error");
			break;
		}

		plen = NLMSG_PAYLOAD(hp, 0);
		ip = (struct ifinfomsg *)NLMSG_DATA(hp);
		if (plen < sizeof(*ip)) {
			FCM_LOG("too short (%d) to be a LINK message", rc);
			break;
		}

		switch (type) {
		case RTM_NEWLINK:
		case RTM_DELLINK:
		case RTM_GETLINK:
			FCM_LOG_DBG("Link event: %d flags %05X index %d ",
				    type, ip->ifi_flags, ip->ifi_index);

			fcm_process_link_msg(ip, plen, type);
			break;

		case RTM_GETDCB:
		case RTM_SETDCB:
			fcm_process_ieee_msg(hp);
			break;

		default:
			FCM_LOG_DBG("%s: Unexpected type %d\n", __func__, type);
			break;
		}
	}
}

/*
 * Send rt_netlink request for all network interfaces.
 */
static void fcm_link_getlink(void)
{
	struct {
		struct nlmsghdr nl;
		struct ifinfomsg ifi;	/* link level specific information,
					   not dependent on network protocol */
	} msg;

	int rc;

	memset(&msg, 0, sizeof(msg));
	msg.nl.nlmsg_len = sizeof(msg);
	msg.nl.nlmsg_type = RTM_GETLINK;
	msg.nl.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT | NLM_F_ATOMIC;
	msg.nl.nlmsg_seq = ++fcm_link_seq;
	msg.nl.nlmsg_pid = getpid();
	msg.ifi.ifi_family = AF_UNSPEC;
	msg.ifi.ifi_type = ARPHRD_ETHER;
	rc = write(fcm_link_socket, &msg, sizeof(msg));
	if (rc < 0)
		FCM_LOG_ERR(errno, "write error");
}

/*
 * Check for whether buffer needs to grow based on amount read.
 * Free's the old buffer so don't use that after this returns non-zero.
 */
static int fcm_link_buf_check(size_t read_len)
{
	char *buf;
	size_t len = read_len;

	if (len > fcm_link_buf_size - fcm_link_buf_fuzz) {
		len = fcm_link_buf_size;
		len = len + len / 2;	/* grow by 50% */
		buf = malloc(len);
		if (buf != NULL) {
			free(fcm_link_buf);
			fcm_link_buf = buf;
			fcm_link_buf_size = len;
			return 1;
		} else {
			FCM_LOG_ERR(errno, "failed to allocate link buffer");
		}
	}
	return 0;
}

static void fcm_fcoe_init(void)
{
	if (fcm_read_config_files())
		exit(1);
}

/*
 * Allocate an FCoE interface state structure.
 */
static struct fcm_netif *fcm_netif_alloc(char *ifname)
{
	struct fcm_netif *ff;

	ff = calloc(1, sizeof(*ff));
	if (ff) {
		snprintf(ff->ifname, sizeof(ff->ifname), "%s", ifname);
		ff->ff_operstate = IF_OPER_UNKNOWN;
		TAILQ_INSERT_TAIL(&fcm_netif_head, ff, ff_list);
	} else {
		FCM_LOG_ERR(errno, "failed to allocate fcm_netif");
	}
	return ff;
}

/*
 * Find or create an FCoE network interface by ifname.
 * @ifname - interface name to create
 *
 * This creates a netif interface structure with interface name,
 * or if one already exists returns the existing one.
 */
static struct fcm_netif *fcm_netif_lookup_create(char *ifname)
{
	struct fcm_netif *ff;

	TAILQ_FOREACH(ff, &fcm_netif_head, ff_list) {
		if (!strncmp(ifname, ff->ifname, IFNAMSIZ))
			return ff;
	}

	ff = fcm_netif_alloc(ifname);
	if (ff != NULL) {
		sa_timer_init(&ff->dcbd_retry_timer, fcm_dcbd_retry_timeout,
			      (void *)ff);
		FCM_LOG_DEV_DBG(ff, "Monitoring port %s\n", ifname);
	}

	return ff;
}

/*
 * Find an FCoE interface by name.
 */
static struct fcm_netif *fcm_netif_lookup(char *ifname)
{
	struct fcm_netif *curr, *ff = NULL;

	TAILQ_FOREACH(curr, &fcm_netif_head, ff_list) {
		if (strcmp(curr->ifname, ifname) == 0) {
			ff = curr;
			break;
		}
	}

	return ff;
}

static void fcm_dcbd_init()
{
	fcm_clif->cl_fd = -1;	/* not connected */
	fcm_clif->cl_ping_pending = 0;
	sa_timer_init(&fcm_dcbd_timer, fcm_dcbd_timeout, NULL);
	fcm_dcbd_timeout(NULL);
}

static int fcm_dcbd_connect(void)
{
	int rc;
	int fd;
	struct sockaddr_un dest;
	struct sockaddr_un *lp;
	socklen_t addrlen;

	ASSERT(fcm_clif->cl_fd < 0);
	fd = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) {
		FCM_LOG_ERR(errno, "clif socket open failed");	/* XXX */
		return 0;
	}

	lp = &fcm_clif->cl_local;
	memset(lp, 0, sizeof(*lp));
	lp->sun_family = AF_LOCAL;
	lp->sun_path[0] = '\0';
	snprintf(&lp->sun_path[1], sizeof(lp->sun_path) - 1,
		 CLIF_LOCAL_SUN_PATH, LLDP_CLIF_SOCK, getpid());
	addrlen = sizeof(sa_family_t) + strlen(lp->sun_path + 1) + 1;
	rc = bind(fd, (struct sockaddr *)lp, addrlen);
	if (rc < 0) {
		FCM_LOG_ERR(errno, "clif bind failed");
		close(fd);
		return 0;
	}

	memset(&dest, 0, sizeof(dest));
	dest.sun_family = AF_LOCAL;
	dest.sun_path[0] = '\0';
	snprintf(&dest.sun_path[1], sizeof(dest.sun_path) - 1,
		 "%s", LLDP_CLIF_SOCK);
	addrlen = sizeof(sa_family_t) + strlen(dest.sun_path + 1) + 1;
	rc = connect(fd, (struct sockaddr *)&dest, addrlen);
	if (rc < 0) {
		FCM_LOG_ERR(errno, "Failed to connect to lldpad");
		close(fd);
		return 0;
	}
	fcm_clif->cl_fd = fd;
	sa_select_add_fd(fd, fcm_dcbd_rx, NULL, NULL, fcm_clif);
	FCM_LOG_DBG("connected to lldpad");
	return 1;
}

static void fcm_dcbd_timeout(void *arg)
{
	if (fcm_clif->cl_ping_pending > 0) {
		fcm_dcbd_request("D");	/* DETACH_CMD */
		fcm_dcbd_disconnect();
	}
	if (fcm_clif->cl_fd < 0) {
		if (fcm_dcbd_connect())
			fcm_dcbd_request("A");	/* ATTACH_CMD: for events */
		else
			sa_timer_set(&fcm_dcbd_timer, DCBD_CONNECT_TIMEOUT);
	} else {
		fcm_clif->cl_ping_pending++;
		fcm_dcbd_request("P");	/* ping to verify connection */
	}
}

static void fcm_dcbd_retry_timeout(void *arg)
{
	struct fcm_netif *ff = (struct fcm_netif *)arg;

	ASSERT(ff);
	FCM_LOG_DBG("%s: lldpad retry TIMEOUT occurred [%d]",
		    ff->ifname, ff->dcbd_retry_cnt);

	fcm_dcbd_state_set(ff, FCD_GET_DCB_STATE);
	fcm_netif_advance(ff);
}

static void fcm_dcbd_disconnect(void)
{
	if (fcm_clif != NULL && fcm_clif->cl_local.sun_path[1] != '\0') {
		if (fcm_clif->cl_fd >= 0) {
			sa_select_rem_fd(fcm_clif->cl_fd);
			close(fcm_clif->cl_fd);
		}
		fcm_clif->cl_local.sun_path[1] = '\0';
		fcm_clif->cl_fd = -1;	/* mark as disconnected */
		fcm_clif->cl_busy = 0;
		fcm_clif->cl_ping_pending = 0;
		FCM_LOG_DBG("Disconnected from lldpad");
	}
}

static void fcm_dcbd_shutdown(void)
{
	FCM_LOG_DBG("Shutdown lldpad connection\n");
	fcm_dcbd_request("D");	/* DETACH_CMD */
	fcm_dcbd_disconnect();
	unlink(fcm_pidfile);
	closelog();
}

static void fcm_cleanup(void)
{
	struct fcoe_port *curr, *next;
	struct fcm_netif *ff, *head;

	for (curr = fcoe_config.port; curr; curr = next) {
		FCM_LOG_DBG("OP: DESTROY %s\n", curr->ifname);
		fcm_fcoe_if_action(FCOE_DESTROY,  curr->ifname);
		next = curr->next;
		free(curr);
	}

	for (head = TAILQ_FIRST(&fcm_netif_head); head; head = ff) {
		ff = TAILQ_NEXT(head, ff_list);
		TAILQ_REMOVE(&fcm_netif_head, head, ff_list);
		free(head);
	}

	free(fcm_link_buf);
}

static u_int32_t fcm_get_hex(char *cp, u_int32_t len, char **endptr)
{
	u_int32_t hex = 0;

	while (len > 0) {
		len--;
		if (*cp >= '0' && *cp <= '9')
			hex = (hex << 4) | (*cp - '0');
		else if (*cp >= 'A' && *cp <= 'F')
			hex = (hex << 4) | (*cp - 'A' + 10);
		else if (*cp >= 'a' && *cp <= 'f')
			hex = (hex << 4) | (*cp - 'a' + 10);
		else
			break;
		cp++;
	}
	*endptr = (len == 0) ? NULL : cp;
	return hex;
}

static void fcm_dcbd_rx(void *arg)
{
	struct fcm_clif *clif = arg;
	cmd_status st;
	char buf[128];
	size_t len;
	int rc;
	char *ep;

	len = sizeof(buf);
	rc = read(clif->cl_fd, buf, sizeof(buf) - 1);
	if (rc < 0)
		FCM_LOG_ERR(errno, "read");
	else if ((rc > 0) && (rc < sizeof(buf))) {
		buf[rc] = '\0';
		len = strlen(buf);
		ASSERT(len <= rc);
		if (len > FCM_PING_RSP_LEN)
			FCM_LOG_DBG("received len %d buf '%s'", len, buf);

		switch (buf[CLIF_RSP_MSG_OFF]) {
		case CMD_RESPONSE:
			st = fcm_get_hex(buf + CLIF_STAT_OFF, CLIF_STAT_LEN,
					 &ep);
			if (ep != NULL)
				FCM_LOG("unexpected response code from lldpad: "
					"len %d buf %s rc %d", len, buf, rc);
			else if (st != cmd_success &&
				 st != cmd_not_applicable &&
				 st != cmd_device_not_found) {
				FCM_LOG("error response from lldpad: "
					"error %d len %d %s",
					st, len, buf);
			}
			fcm_clif->cl_busy = 0;

			switch (buf[3]) {
			case DCB_CMD:
				fcm_dcbd_cmd_resp(buf, st);
				break;
			case ATTACH_CMD:
				break;
			case DETACH_CMD:
				break;
			case PING_CMD:
				if (clif->cl_ping_pending > 0)
					--clif->cl_ping_pending;
				break;
			case LEVEL_CMD:
				break;
			default:
				FCM_LOG("Unexpected cmd in response "
					"from lldpad: len %d %s",
					len, buf);
				break;
			}
			break;

		case EVENT_MSG:
			fcm_dcbd_event(buf, len);
			break;
		default:
			FCM_LOG("Unexpected message from lldpad: len %d buf %s",
				len, buf);
			break;
		}
	}
}

/*
 * returns:  1 if request was successfully written
 *           0 if the write failed
 */
static int fcm_dcbd_request(char *req)
{
	size_t len;
	int rc;

	if (fcm_clif->cl_fd < 0)
		return 0;
	len = strlen(req);
	ASSERT(fcm_clif->cl_busy == 0);
	sa_timer_set(&fcm_dcbd_timer, DCBD_CONNECT_TIMEOUT);
	fcm_clif->cl_busy = 1;
	rc = write(fcm_clif->cl_fd, req, len);
	if (rc < 0) {
		FCM_LOG_ERR(errno, "Failed write req %s len %d", req, len);
		fcm_clif->cl_busy = 0;
		fcm_dcbd_disconnect();
		sa_timer_set(&fcm_dcbd_timer, DCBD_CONNECT_RETRY_TIMEOUT);
		return 0;
	}

	if (rc > FCM_PING_REQ_LEN)
		FCM_LOG_DBG("sent '%s', rc=%d bytes succeeded", req, rc);
	return 1;
}

/*
 * Find port for message.
 * The port name length starts at len_off for len_len bytes.
 * The entire message length is len.
 * The pointer to the message pointer is passed in, and updated to point
 * past the interface name.
 */
static struct fcm_netif *fcm_dcbd_get_port(char **msgp, size_t len_off,
					   size_t len_len, size_t len)
{
	struct fcm_netif *ff;
	u_int32_t if_len;
	char *ep;
	char *msg;
	char ifname[IFNAMSIZ];

	msg = *msgp;
	if (len_off + len_len >= len)
		return NULL;

	if_len = fcm_get_hex(msg + len_off, len_len, &ep);
	if (ep != NULL) {
		FCM_LOG("Parse error on port len: msg %s", msg);
		return NULL;
	}

	if (len_off + len_len + if_len > len) {
		FCM_LOG("Invalid port len %d msg %s", if_len, msg);
		return NULL;
	}
	msg += len_off + len_len;
	sa_strncpy_safe(ifname, sizeof(ifname), msg, if_len);
	*msgp = msg + if_len;
	ff = fcm_netif_lookup(ifname);
	if (ff == NULL) {
		FCM_LOG("ifname '%s' not found", ifname);
	}
	return ff;
}

/*
 * (XXX) Notes:
 * This routine is here to help fcm_dcbd_cmd_resp() to pick up
 * information of the response packet from the DCBD.
 * Returns:  0 on success
 *          -1 on failure
 */
static int dcb_rsp_parser(struct fcm_netif *ff, char *rsp)
{
	int version;
	int dcb_cmd;
	int feature;
	int subtype;
	int plen;
	int doff;
	int i;
	int n;
	struct feature_info *f_info = NULL;
	char buf[20];

	feature = hex2int(rsp+DCB_FEATURE_OFF);

	dcb_cmd = hex2int(rsp+DCB_CMD_OFF);

	version = rsp[DCB_VER_OFF] & 0x0f;
	if (version != CLIF_MSG_VERSION) {
		FCM_LOG_DEV(ff, "WARNING: Unexpected rsp version %d\n",
			    version);
		return -1;
	}

	subtype = hex2int(rsp+DCB_SUBTYPE_OFF);
	plen = hex2int(rsp+DCB_PORTLEN_OFF);
	doff = DCB_PORT_OFF + plen;

	switch (feature) {
	case FEATURE_DCB:
		ff->ff_dcb_state = (*(rsp+doff+CFG_ENABLE) == '1');
		return 0;
	case FEATURE_PFC:
		f_info = &ff->ff_pfc_info;
		break;
	case FEATURE_APP:
		f_info = &ff->ff_app_info;
		f_info->subtype = subtype;
		break;
	default:
		return -1;
	}

	switch (dcb_cmd) {
	case CMD_GET_CONFIG:
		f_info->enable = (*(rsp+doff+CFG_ENABLE) == '1');
		f_info->advertise = (*(rsp+doff+CFG_ADVERTISE) == '1');
		f_info->willing = (*(rsp+doff+CFG_WILLING) == '1');
		doff += CFG_LEN;
		break;

	case CMD_GET_OPER:
		f_info->op_vers = hex2int(rsp+doff+OPER_OPER_VER);
		f_info->op_error = hex2int(rsp+doff+OPER_ERROR);
		f_info->op_mode = (*(rsp+doff+OPER_OPER_MODE) == '1');
		f_info->syncd = (*(rsp+doff+OPER_SYNCD) == '1');
		doff += OPER_LEN;
		if (feature == FEATURE_PFC) {
			f_info->u.pfcup = 0;
			for (i = 0; i < MAX_USER_PRIORITIES; i++) {
				if (*(rsp+doff+PFC_UP(i)) == '1')
					f_info->u.pfcup |= 1<<i;
			}
		}
		if (feature == FEATURE_APP && subtype == APP_FCOE_STYPE) {
			n = hex2int(rsp+doff+APP_LEN);
			snprintf(buf, sizeof(buf), "%*.*s\n",
				 n, n, rsp+doff+APP_DATA);
			f_info->u.appcfg = hex2int(buf);
		}
		break;
	}

	return 0;
}

/*
 * validate_ieee_info - Validation IEEE DCB status for FCoE
 *
 * Returns:  FCP_ACTIVATE_IF - if the dcb netif qualifies for an fcoe interface
 *           FCP_DESTROY_IF - if the dcb netif should not support fcoe interface
 *           FCP_ERROR - if dcb configuration has errors
 *           FCP_WAIT - if dcb criteria is inconclusive
 */
static enum fcp_action validate_ieee_info(struct fcm_netif *ff)
{
	if (ff->ieee_pfc_info & ff->ieee_app_info) {
		FCM_LOG_DBG("%s: %s: IEEE active and valid\n",
			__func__, ff->ifname);
		return FCP_ACTIVATE_IF;
	}
	FCM_LOG_DBG("%s: %s: IEEE active and invalid, pfc=0x%x, app=0x%x\n",
		    __func__, ff->ifname, ff->ieee_pfc_info, ff->ieee_app_info);
	return FCP_WAIT;
}

/*
 * validate_dcbd_info - Validating DCBD configuration and status
 *
 * Returns:  FCP_ACTIVATE_IF - if the dcb netif qualifies for an fcoe interface
 *           FCP_DESTROY_IF - if the dcb netif should not support fcoe interface
 *           FCP_ERROR - if dcb configuration has errors
 *           FCP_WAIT - if dcb criteria is inconclusive
 */
static enum fcp_action validate_dcbd_info(struct fcm_netif *ff)
{
	int errors = 0;
	int dcbon;

	dcbon = ff->ff_dcb_state;
	if (dcbon && (ff->dcbx_cap & DCB_CAP_DCBX_VER_IEEE))
		return validate_ieee_info(ff);

	/* check if dcb state qualifies to create the fcoe interface */
	if (dcbon &&
	    ff->ff_app_info.enable &&
	    ff->ff_pfc_info.enable &&
	    ff->ff_app_info.op_mode &&
	    ff->ff_pfc_info.op_mode &&
	    ff->ff_pfc_info.u.pfcup & ff->ff_app_info.u.appcfg) {

		if (dcbon && !ff->ff_app_info.willing) {
			FCM_LOG_DEV(ff,
				    "WARNING: FCoE willing mode is false\n");
			errors++;
		}
		if (dcbon && !ff->ff_app_info.advertise) {
			FCM_LOG_DEV(ff,
				    "WARNING: FCoE advertise mode is false\n");
			errors++;
		}
		if (dcbon && !ff->ff_pfc_info.willing) {
			FCM_LOG_DEV(ff,
				    "WARNING: PFC willing mode is false\n");
			errors++;
		}
		if (dcbon && !ff->ff_pfc_info.advertise) {
			FCM_LOG_DEV(ff,
				    "WARNING: PFC advertise mode is false\n");
			errors++;
		}

		if (errors)
			FCM_LOG_DEV_DBG(ff,
					"WARNING: DCB may not be configured correctly\n");
		else
			FCM_LOG_DEV_DBG(ff, "DCB is configured correctly\n");

		return FCP_ACTIVATE_IF;
	}

	/* check if dcb state qualifies to destroy the fcoe interface */
	if (!dcbon ||
	    !ff->ff_app_info.enable ||
	    (ff->ff_app_info.op_mode && ff->ff_pfc_info.op_mode &&
	     !(ff->ff_pfc_info.u.pfcup & ff->ff_app_info.u.appcfg))) {

		if (dcbon && !ff->ff_dcb_state)
			FCM_LOG_DEV(ff, "WARNING: DCB is disabled\n");

		if (dcbon && !ff->ff_app_info.enable)
			FCM_LOG_DEV(ff, "WARNING: FCoE enable is off\n");

		if (dcbon &&
		    !(ff->ff_pfc_info.u.pfcup & ff->ff_app_info.u.appcfg))
			FCM_LOG_DEV(ff,
				    "WARNING: FCoE priority (0x%02x) doesn't "
				    "intersect with PFC priority (0x%02x)\n",
				    ff->ff_app_info.u.appcfg,
				    ff->ff_pfc_info.u.pfcup);

		return FCP_DISABLE_IF;
	}

	/* The dcbd state does not match the create or destroy criteria.
	 * Log possible problems.
	 */
	if (dcbon && !ff->ff_app_info.willing) {
		FCM_LOG_DEV(ff, "WARNING: FCoE willing mode is false\n");
		errors++;
	}
	if (dcbon && !ff->ff_app_info.advertise) {
		FCM_LOG_DEV(ff, "WARNING: FCoE advertise mode is false\n");
		errors++;
	}
	if (dcbon && !ff->ff_app_info.op_mode) {
		FCM_LOG_DEV(ff, "WARNING: FCoE operational mode is false\n");
		print_errors(ff->ff_app_info.op_error);
		errors++;
	}
	if (dcbon && !ff->ff_pfc_info.enable) {
		FCM_LOG_DEV(ff, "WARNING: PFC enable is off\n");
		errors++;
	}
	if (dcbon && !ff->ff_pfc_info.advertise) {
		FCM_LOG_DEV(ff, "WARNING: PFC advertise mode is false\n");
		errors++;
	}
	if (dcbon && !ff->ff_app_info.op_mode) {
		FCM_LOG_DEV(ff, "WARNING: APP:0 operational mode is false\n");
		print_errors(ff->ff_app_info.op_error);
		errors++;
	}
	if (dcbon && !ff->ff_pfc_info.op_mode) {
		FCM_LOG_DEV(ff, "WARNING: PFC operational mode is false\n");
		print_errors(ff->ff_pfc_info.op_error);
		errors++;
	}
	if (dcbon && !(ff->ff_pfc_info.u.pfcup & ff->ff_app_info.u.appcfg)) {
		FCM_LOG_DEV(ff, "WARNING: APP:0 priority (0x%02x) doesn't "
			    "intersect with PFC priority (0x%02x)\n",
			    ff->ff_app_info.u.appcfg,
			    ff->ff_pfc_info.u.pfcup);
		errors++;
	}
	if (errors) {
		FCM_LOG_DEV(ff, "WARNING: DCB may be configured incorrectly\n");
		return FCP_ERROR;
	}

	return FCP_WAIT;
}

/*
 * clear_dcbd_info - clear dcbd info to unknown values
 *
 */
static void clear_dcbd_info(struct fcm_netif *ff)
{
	memset(&ff->ff_pfc_info, 0, sizeof(struct feature_info));
	memset(&ff->ff_app_info, 0, sizeof(struct feature_info));
}


/**
 * fcm_dcbd_set_config() - Response handler for set config command
 * @ff: fcoe port structure
 * @st: status
 */
static void fcm_dcbd_set_config(struct fcm_netif *ff)
{
	if (ff->ff_dcbd_state == FCD_SEND_CONF)
		fcm_dcbd_state_set(ff, FCD_GET_PFC_CONFIG);
}

/**
 * fcm_dcbd_get_config() - Response handler for get config command
 * @ff:   fcoe port structure
 * @resp: response buffer
 * @st:   status
 */
static void fcm_dcbd_get_config(struct fcm_netif *ff, char *resp)
{
	switch (ff->ff_dcbd_state) {
	case FCD_GET_DCB_STATE:
		if (!dcb_rsp_parser(ff, resp)) {
			if (ff->ff_dcb_state &&
			    !(ff->dcbx_cap & DCB_CAP_DCBX_VER_IEEE))
				fcm_dcbd_state_set(ff, FCD_GET_PFC_CONFIG);
			else
				fcm_dcbd_state_set(ff, FCD_DONE);
		} else
			fcm_dcbd_state_set(ff, FCD_ERROR);
		break;
	case FCD_GET_PFC_CONFIG:
		if (!dcb_rsp_parser(ff, resp))
			fcm_dcbd_state_set(ff, FCD_GET_APP_CONFIG);
		else
			fcm_dcbd_state_set(ff, FCD_ERROR);
		break;
	case FCD_GET_APP_CONFIG:
		if (!dcb_rsp_parser(ff, resp))
			fcm_dcbd_state_set(ff, FCD_GET_PFC_OPER);
		else
			fcm_dcbd_state_set(ff, FCD_ERROR);
		break;
	default:
		break;
	}
}


/**
 * fcm_dcbd_get_oper() - Response handler for get operational state command
 * @ff:   fcoe port structure
 * @resp: response buffer
 * @cp:   response buffer pointer, points past the interface name
 * @st:   status
 *
 * Sample msg: R00C103050004eth8010100100208
 *                  opppssll    vvmmeemsllpp
 */
static void fcm_dcbd_get_oper(struct fcm_netif *ff, char *resp, char *cp)
{
	u_int32_t val;
	char *ep = NULL;

	val = fcm_get_hex(cp + OPER_ERROR, 2, &ep);

	if (ep) {
		FCM_LOG_DEV(ff, "Invalid get oper response "
			    "parse error byte %d, resp %s", ep - cp, cp);
		fcm_dcbd_state_set(ff, FCD_ERROR);
	} else {
		if (val && fcoe_config.debug)
			print_errors(val);

		switch (ff->ff_dcbd_state) {
		case FCD_GET_PFC_OPER:
			if (dcb_rsp_parser(ff, resp) || !ff->ff_pfc_info.syncd)
				fcm_dcbd_state_set(ff, FCD_ERROR);
			else
				fcm_dcbd_state_set(ff, FCD_GET_APP_OPER);

			FCM_LOG_DEV_DBG(ff, "PFC feature is %ssynced",
					ff->ff_pfc_info.syncd ? "" : "not ");
			FCM_LOG_DEV_DBG(ff, "PFC operating mode is %s",
					ff->ff_pfc_info.op_mode ? "on" :
					"off ");
			break;

		case FCD_GET_APP_OPER:
			if (dcb_rsp_parser(ff, resp) || !ff->ff_app_info.syncd)
				fcm_dcbd_state_set(ff, FCD_ERROR);
			else
				fcm_dcbd_state_set(ff, FCD_DONE);

			FCM_LOG_DEV_DBG(ff, "FCoE feature is %ssynced",
					ff->ff_app_info.syncd ? "" :
					"not ");
			FCM_LOG_DEV_DBG(ff, "FCoE operating mode is %s",
					ff->ff_app_info.op_mode ? "on" :
					"off ");
			break;

		default:
			break;
		}
	}
}

/*
 * Handle command response.
 * Response buffer points past command code character in response.
 */
static void fcm_dcbd_cmd_resp(char *resp, cmd_status st)
{
	struct fcm_netif *ff;
	u_int32_t ver;
	u_int32_t cmd;
	u_int32_t feature;
	u_int32_t subtype;
	u_int32_t state;
	char *ep;
	char *cp;
	size_t len;

	resp += CLIF_RSP_OFF;
	len = strlen(resp);
	ver = fcm_get_hex(resp + DCB_VER_OFF, DCB_VER_LEN, &ep);
	if (ep != NULL) {
		FCM_LOG("parse error: resp %s", resp);
		return;
	} else	if (ver != CLIF_RSP_VERSION) {
		FCM_LOG("unexpected version %d resp %s", ver, resp);
		return;
	}
	cmd = fcm_get_hex(resp + DCB_CMD_OFF, DCB_CMD_LEN, &ep);
	if (ep != NULL) {
		FCM_LOG("parse error on resp cmd: resp %s", resp);
		return;
	}
	feature = fcm_get_hex(resp + DCB_FEATURE_OFF, DCB_FEATURE_LEN, &ep);
	if (ep != NULL) {
		FCM_LOG("parse error on resp feature: resp %s", resp);
		return;
	}
	subtype = fcm_get_hex(resp + DCB_SUBTYPE_OFF, DCB_SUBTYPE_LEN, &ep);
	if (ep != NULL) {
		FCM_LOG("parse error on resp subtype: resp %s", resp);
		return;
	}
	cp = resp;
	ff = fcm_dcbd_get_port(&cp, DCB_PORTLEN_OFF, DCB_PORTLEN_LEN, len);
	if (ff == NULL) {
		FCM_LOG("port not found. resp %s", resp);
		return;
	}

	/*
	 * check that dcbd response matches the current dcbd state.
	 */
	state = ff->ff_dcbd_state;
	if (((cmd == CMD_GET_CONFIG) &&
	     ((state == FCD_GET_DCB_STATE && feature == FEATURE_DCB) ||
	      (state == FCD_GET_PFC_CONFIG && feature == FEATURE_PFC) ||
	      (state == FCD_GET_APP_CONFIG && feature == FEATURE_APP)))
	    ||
	    ((cmd == CMD_GET_OPER) &&
	     ((state == FCD_GET_PFC_OPER && feature == FEATURE_PFC) ||
	      (state == FCD_GET_APP_OPER && feature == FEATURE_APP)))) {

		/* the response matches the current pending query */
		ff->response_pending = 0;
		if (st != cmd_success) {
			if (st == cmd_not_applicable)
				fcm_dcbd_state_set(ff, FCD_DONE);
			else
				fcm_dcbd_state_set(ff, FCD_ERROR);
			return;
		}
	}

	switch (cmd) {
	case CMD_SET_CONFIG:
		fcm_dcbd_set_config(ff);
		break;

	case CMD_GET_CONFIG:
		fcm_dcbd_get_config(ff, resp);
		break;

	case CMD_GET_OPER:
		fcm_dcbd_get_oper(ff, resp, cp);
		break;

	default:
		FCM_LOG_DEV_DBG(ff, "Unknown cmd 0x%x in response: resp %s",
				cmd, resp);
		break;
	}
}

/*
 * Handle incoming DCB event message.
 * Example message: E5104eth8050001
 */
static void fcm_dcbd_event(char *msg, size_t len)
{
	struct fcm_netif *ff;
	struct fcoe_port *p;
	u_int32_t feature;
	u_int32_t subtype;
	char *cp;
	char *ep;

	if (msg[EV_LEVEL_OFF] != MSG_DCB + '0' || len <= EV_PORT_ID_OFF)
		return;
	if (msg[EV_VERSION_OFF] != CLIF_EV_VERSION + '0') {
		FCM_LOG("Unexpected version in event msg %s", msg);
		return;
	}
	cp = msg;
	ff = fcm_dcbd_get_port(&cp, EV_PORT_LEN_OFF, EV_PORT_LEN_LEN, len);
	if (ff == NULL)
		return;

	feature = fcm_get_hex(cp + EV_FEATURE_OFF, 2, &ep);
	if (ep != NULL) {
		FCM_LOG_DEV_DBG(ff, "Invalid feature code in event msg %s",
				msg);
		return;
	}

	/*
	 * Check if the FCoE ports which use the interface on which the
	 * dcbd event arrived are configured to require dcb.
	 */

	p = fcm_find_fcoe_port(ff->ifname, FCP_REAL_IFNAME);
	while (p) {
		if (p->dcb_required && p->last_msg_type != RTM_DELLINK &&
		    p->fcoe_enable)
			break;
		p = fcm_find_next_fcoe_port(p, ff->ifname);
	}

	/*
	 * dcb is not required or link was removed, ignore dcbd event
	 */
	if (!p)
		return;

	if (ff->ff_operstate != IF_OPER_UP)
		return;

	switch (feature) {
	case FEATURE_PG:     /* 'E5204eth2020001' */
		FCM_LOG_DEV_DBG(ff, "<Got PG Event>\n");
		break;
	case FEATURE_PFC:    /* 'E5204eth2030011' */
		FCM_LOG_DEV_DBG(ff, "<Got PFC Event>\n");
		fcm_dcbd_state_set(ff, FCD_GET_DCB_STATE);
		break;
	case FEATURE_APP:    /* 'E5204eth2050011' */
		FCM_LOG_DEV_DBG(ff, "<Got APP Event>\n");
		subtype = fcm_get_hex(cp + EV_SUBTYPE_OFF, 2, &ep);
		if (subtype != APP_FCOE_STYPE) {
			FCM_LOG_DEV_DBG(ff, "Unknown application subtype "
					"in msg %s", msg);
			break;
		}
		fcm_dcbd_state_set(ff, FCD_GET_DCB_STATE);
		break;
	default:
		FCM_LOG_DEV_DBG(ff, "Unknown feature 0x%x in msg %s",
				feature, msg);
		break;
	}

	if (fcoe_config.debug) {
		if (cp[EV_OP_MODE_CHG_OFF] == '1')
			FCM_LOG_DEV_DBG(ff,
					"Operational mode changed");
		if (cp[EV_OP_CFG_CHG_OFF] == '1')
			FCM_LOG_DEV_DBG(ff,
					"Operational config changed");
	}
}

static void fcm_cli_reply(struct sock_info *r, int status)
{
	char rbuf[MAX_MSGBUF];
	snprintf(rbuf, MSG_RBUF, "%d", status);
	sendto(r->sock, rbuf, MSG_RBUF, 0, (struct sockaddr *)&(r->from),
	       r->fromlen);
}

static enum fcoe_status fcm_fcoe_if_action(char *path, char *ifname)
{
	FILE *fp = NULL;
	enum fcoe_status ret = EFAIL;

	fp = fopen(path, "w");
	if (!fp) {
		FCM_LOG_ERR(errno, "%s: Failed to open path %s\n",
			    progname, path);
		goto err_out;
	}

	if (EOF == fputs(ifname, fp)) {
		FCM_LOG_ERR(errno, "%s: Failed to write %s to path %s.\n",
			    progname, ifname, path);
		goto out;
	}

	ret = SUCCESS;
out:
	fclose(fp);
err_out:
	return ret;
}

void fcm_vlan_disc_timeout(void *arg)
{
	struct fcoe_port *p = arg;
	FCM_LOG_DBG("%s: VLAN discovery TIMEOUT [%d]",
		    p->ifname, p->vlan_disc_count);
	p->vlan_disc_count++;
	if (!(p->fcoe_enable && p->auto_vlan) &&
			(p->vlan_disc_count > FCM_VLAN_DISC_MAX)) {
		FCM_LOG("%s: VLAN discovery failed after %d attempts",
			p->ifname, FCM_VLAN_DISC_MAX);
		FCM_LOG("%s: disabling VLAN discovery, trying FCoE on %s",
			p->ifname, p->ifname);
		p->auto_vlan = 0;
		fcp_set_next_action(p, FCP_ACTIVATE_IF);
		return;
	}
	fip_send_vlan_request(p->fip_socket, p->ifindex, p->mac);
	sa_timer_set(&p->vlan_disc_timer, FCM_VLAN_DISC_TIMEOUT);
}

int fcm_start_vlan_disc(struct fcoe_port *p)
{
	int s;
	if (p->fip_socket < 0) {
		s = fcm_vlan_disc_socket(p);
		if (s < 0)
			return s;
		p->fip_socket = s;
	}
	p->vlan_disc_count = 1;
	fip_send_vlan_request(p->fip_socket, p->ifindex, p->mac);
	sa_timer_set(&p->vlan_disc_timer, FCM_VLAN_DISC_TIMEOUT);
	return 0;
}

/*
 *
 * Input:  action = 1      Destroy the FCoE interface
 *         action = 2      Create the FCoE interface
 *         action = 3      Reset the interface
 */
static void fcm_fcoe_action(struct fcm_netif *ff, struct fcoe_port *p)
{
	struct fcoe_port *vp;
	char *ifname = p->ifname;
	char path[256];
	enum fcoe_status rc = SUCCESS;

	switch (p->action) {
	case FCP_CREATE_IF:
		FCM_LOG_DBG("OP: CREATE %s\n", p->ifname);
		rc = fcm_fcoe_if_action(FCOE_CREATE, ifname);
		/*
		 * This call validates that the interface name
		 * has an active fcoe session by checking for
		 * the fc_host in sysfs.
		 */
		if (fcoe_find_fchost(ifname, p->fchost, FCHOSTBUFLEN))
			FCM_LOG_DBG("filed to get fchost for %s\n", p->ifname);

		FCM_LOG_DBG("OP: created fchost:%s for %s\n",
			     p->fchost, p->ifname);
		break;
	case FCP_DESTROY_IF:
		FCM_LOG_DBG("OP: DESTROY %s\n", p->ifname);
		if (p->auto_vlan) {
			/* destroy all the VLANs */
			vp = fcm_find_fcoe_port(p->ifname, FCP_REAL_IFNAME);
			while (vp) {
				if (vp->auto_created) {
					vp->fcoe_enable = 0;
					fcp_set_next_action(vp, FCP_DESTROY_IF);
				}
				vp = fcm_find_next_fcoe_port(vp, p->ifname);
			}
			rc = SUCCESS;
			break;
		}
		rc = fcm_fcoe_if_action(FCOE_DESTROY, ifname);
		p->fchost[0] = '\0';
		break;
	case FCP_ENABLE_IF:
		FCM_LOG_DBG("OP: ENABLE %s\n", p->ifname);
		rc = fcm_fcoe_if_action(FCOE_ENABLE, ifname);
		break;
	case FCP_DISABLE_IF:
		FCM_LOG_DBG("OP: DISABLE %s\n", p->ifname);
		if (p->auto_vlan) {
			/* disable all the VLANs */
			vp = fcm_find_fcoe_port(p->ifname, FCP_REAL_IFNAME);
			while (vp) {
				if (vp->auto_created)
					fcp_set_next_action(vp, FCP_DISABLE_IF);
				vp = fcm_find_next_fcoe_port(vp, p->ifname);
			}
			break;
		}
		rc = fcm_fcoe_if_action(FCOE_DISABLE, ifname);
		break;
	case FCP_RESET_IF:
		FCM_LOG_DBG("OP: RESET %s\n", p->ifname);

		if (strlen(p->fchost) <= 0)  {
			fcm_cli_reply(p->sock_reply, ENOFCHOST);
			return;
		}

		sprintf(path, "%s/%s/issue_lip", SYSFS_FCHOST, p->fchost);
		FCM_LOG_DBG("OP: RESET %s\n", path);
		rc = fcm_fcoe_if_action(path, "1");
		break;
	case FCP_SCAN_IF:
		FCM_LOG_DBG("OP: SCAN %s\n", p->ifname);
		if (strlen(p->fchost) <= 0)  {
			fcm_cli_reply(p->sock_reply, ENOFCHOST);
			return;
		}

		sprintf(path, "%s/%s/device/scsi_host/%s/scan",
			SYSFS_FCHOST, p->fchost, p->fchost);
		FCM_LOG_DBG("OP: SCAN %s\n", path);
		rc = fcm_fcoe_if_action(path, "- - -");
		break;
	case FCP_VLAN_DISC:
		FCM_LOG_DBG("OP: VLAN DISC %s\n", p->ifname);
		rc = fcm_start_vlan_disc(p);
		break;
	default:
		return;
		break;
	}

	if (p->sock_reply) {
		fcm_cli_reply(p->sock_reply, rc);
		free(p->sock_reply);
		p->sock_reply = NULL;
	}

	p->last_action = p->action;
}

/*
 * Called for all ports.  For FCoE ports and candidates,
 * get information and send to dcbd.
 */
static void fcm_netif_advance(struct fcm_netif *ff)
{
	char buf[80], params[30];

	ASSERT(ff);
	ASSERT(fcm_clif);

	if (fcm_clif->cl_busy)
		return;

	if (ff->response_pending)
		return;

	if (sa_timer_active(&ff->dcbd_retry_timer))
		return;

	switch (ff->ff_dcbd_state) {
	case FCD_INIT:
	case FCD_ERROR:
		break;
	case FCD_GET_DCB_STATE:
		snprintf(buf, sizeof(buf), "%c%x%2.2x%2.2x%2.2x%2.2x%s",
			 DCB_CMD, CLIF_RSP_VERSION,
			 CMD_GET_CONFIG, FEATURE_DCB, 0,
			 (u_int) strlen(ff->ifname), ff->ifname);
		ff->response_pending = fcm_dcbd_request(buf);
		break;
	case FCD_SEND_CONF:
		snprintf(params, sizeof(params), "%x1%x02",
			 ff->ff_app_info.enable,
			 ff->ff_app_info.willing);
		snprintf(buf, sizeof(buf), "%c%x%2.2x%2.2x%2.2x%2.2x%s%s",
			 DCB_CMD, CLIF_RSP_VERSION,
			 CMD_SET_CONFIG, FEATURE_APP, APP_FCOE_STYPE,
			 (u_int) strlen(ff->ifname), ff->ifname, params);
		ff->response_pending = fcm_dcbd_request(buf);
		break;
	case FCD_GET_PFC_CONFIG:
		snprintf(buf, sizeof(buf), "%c%x%2.2x%2.2x%2.2x%2.2x%s%s",
			 DCB_CMD, CLIF_RSP_VERSION,
			 CMD_GET_CONFIG, FEATURE_PFC, 0,
			 (u_int) strlen(ff->ifname), ff->ifname, "");
		ff->response_pending = fcm_dcbd_request(buf);
		break;
	case FCD_GET_APP_CONFIG:
		snprintf(buf, sizeof(buf), "%c%x%2.2x%2.2x%2.2x%2.2x%s%s",
			 DCB_CMD, CLIF_RSP_VERSION,
			 CMD_GET_CONFIG, FEATURE_APP, APP_FCOE_STYPE,
			 (u_int) strlen(ff->ifname), ff->ifname, "");
		ff->response_pending = fcm_dcbd_request(buf);
		break;
	case FCD_GET_PFC_OPER:
		snprintf(buf, sizeof(buf), "%c%x%2.2x%2.2x%2.2x%2.2x%s%s",
			 DCB_CMD, CLIF_RSP_VERSION,
			 CMD_GET_OPER, FEATURE_PFC, 0,
			 (u_int) strlen(ff->ifname), ff->ifname, "");
		ff->response_pending = fcm_dcbd_request(buf);
		break;
	case FCD_GET_APP_OPER:
		snprintf(buf, sizeof(buf), "%c%x%2.2x%2.2x%2.2x%2.2x%s%s",
			 DCB_CMD, CLIF_RSP_VERSION,
			 CMD_GET_OPER, FEATURE_APP, APP_FCOE_STYPE,
			 (u_int) strlen(ff->ifname), ff->ifname, "");
		ff->response_pending = fcm_dcbd_request(buf);
		break;
	case FCD_GET_PEER:
		snprintf(buf, sizeof(buf), "%c%x%2.2x%2.2x%2.2x%2.2x%s%s",
			 DCB_CMD, CLIF_RSP_VERSION,
			 CMD_GET_PEER, FEATURE_APP, APP_FCOE_STYPE,
			 (u_int) strlen(ff->ifname), ff->ifname, "");
		ff->response_pending = fcm_dcbd_request(buf);
		break;
	case FCD_DONE:
		switch (validate_dcbd_info(ff)) {
		case FCP_DESTROY_IF:
			fcp_action_set(ff->ifname, FCP_DESTROY_IF);
			fcm_dcbd_state_set(ff, FCD_INIT);
			break;
		case FCP_DISABLE_IF:
			fcp_action_set(ff->ifname, FCP_DISABLE_IF);
			fcm_dcbd_state_set(ff, FCD_INIT);
			break;
		case FCP_ACTIVATE_IF:
			fcp_action_set(ff->ifname, FCP_ACTIVATE_IF);
			fcm_dcbd_state_set(ff, FCD_INIT);
			break;
		case FCP_ERROR:
			fcp_action_set(ff->ifname, FCP_DISABLE_IF);
			if (ff->dcbd_retry_cnt < DCBD_MAX_REQ_RETRIES)
				fcm_dcbd_state_set(ff, FCD_ERROR);
			else
				fcm_dcbd_state_set(ff, FCD_INIT);
			break;
		case FCP_WAIT:
		default:
			break;
		}

		break;
	default:
		break;
	}
}

/*
 * Run through these steps at the end of each select loop.
 * 1.  Process list of network interfaces
 *     - issue next dcbd query action
 *     - if query sequence is complete - update FCoE port objects
 *       as necessary with a CREATE or DESTROY next action.
 * 2.  Process FCoE port list - handle next actions, update states, clean up
 */
static void fcm_handle_changes()
{
	struct fcm_netif *ff;
	struct fcoe_port *p;
	int i;

	/*
	 * Perform pending actions (dcbd queries) on network interfaces.
	 */
	TAILQ_FOREACH(ff, &fcm_netif_head, ff_list)
		fcm_netif_advance(ff);

	/*
	 * Perform actions on FCoE ports
	 */
	i = 0;
	p = fcoe_config.port;
	while (p) {
		ff = fcm_netif_lookup(p->real_ifname);
		if (!ff) {
			if (p->sock_reply) {
				fcm_cli_reply(p->sock_reply, ENOETHDEV);
				free(p->sock_reply);
				p->sock_reply = NULL;
				p->action = FCP_WAIT;
			}
			goto next_port;
		}

		fcm_fcoe_action(ff, p);

		fcp_set_next_action(p, FCP_WAIT);
next_port:
		p = p->next;
	}
}

static void fcm_usage(void)
{
	printf("Usage: %s\n"
	       "\t [-f|--foreground]\n"
	       "\t [-d|--debug]\n"
	       "\t [-s|--syslog]\n"
	       "\t [-v|--version]\n"
	       "\t [-h|--help]\n\n", progname);
	exit(1);
}

static void fcm_sig(int sig)
{
	sa_select_exit(sig);
}

static void fcm_pidfile_create(void)
{
	FILE *fp;
	char buf[100];
	char *sp;
	int pid;
	int rc;

	fp = fopen(fcm_pidfile, "r+");
	if (fp) {
		if ((sp = fgets(buf, sizeof(buf), fp)) == NULL) {
			FCM_LOG("Error reading pid file - exiting\n");
			exit(1);
		}
		if (!sscanf(sp, "%d", &pid)) {
			FCM_LOG("Error reading pid ('%s') - exiting\n",
				pid);
			exit(1);
		}
		rc = kill(pid, 0);
		if (pid > 0 && !rc) {
			FCM_LOG("Another instance"
				" (pid %d) is running - exiting\n",
				pid);
			exit(1);
		}
		fclose(fp);
	}
	umask(~(S_IRUSR | S_IWUSR));
	fp = fopen(fcm_pidfile, "w+");
	if (fp) {
		fprintf(fp, "%d\n", getpid());
		fclose(fp);
	}
}

/*
 * TODO: This routine does too much. It executes a 'cmd'
 * and allocates a fcoe_port if one doesn't exist. The
 * function name implies that it only does the latter.
 */
static struct fcoe_port *fcm_port_create(char *ifname, int cmd)
{
	struct fcoe_port *p;
	struct fcoe_port *curr;
	struct fcm_netif *ff;

	p = fcm_find_fcoe_port(ifname, FCP_CFG_IFNAME);
	if (p) {
		if (!p->fcoe_enable) {
			p->fcoe_enable = 1;
			fcp_set_next_action(p, cmd);
			if (p->dcb_required) {
				ff = fcm_netif_lookup(p->real_ifname);
				if (!ff)
					return p;
				fcm_dcbd_state_set(ff, FCD_GET_DCB_STATE);
				if (ff->ff_dcbd_state == FCD_GET_DCB_STATE)
					fcp_set_next_action(p, FCP_WAIT);
			}
		} else {
			p->fcoe_enable = 1;
			fcp_set_next_action(p, cmd);
		}
		return p;
	}

	p = alloc_fcoe_port(ifname);
	if (!p) {
		FCM_LOG_ERR(errno, "fail to allocate fcoe_port %s", ifname);
		return NULL;
	}

	fcm_vlan_dev_real_dev(ifname, p->real_ifname);
	if (!strlen(p->real_ifname))
		snprintf(p->real_ifname, sizeof(p->real_ifname), "%s", ifname);
	p->fcoe_enable = 1;
	p->dcb_required = 0;
	fcp_set_next_action(p, cmd);
	p->next = NULL;

	if (!fcoe_config.port)
		fcoe_config.port = p;
	else {
		curr = fcoe_config.port;
		while (curr->next)
			curr = curr->next;
		curr->next = p;
	}

	/* check and add the real_ifname to the network interface list */
	ff = fcm_netif_lookup_create(p->real_ifname);
	if (!ff) {
		FCM_LOG_ERR(errno, "fail to allocate fcm_netif %s", ifname);
		return NULL;
	}
	return p;
}

static enum fcoe_status fcm_cli_create(char *ifname,
				       struct sock_info **r)
{
	struct fcoe_port *p, *vp;
	enum fcoe_status rc = EFAIL;

	p = fcm_find_fcoe_port(ifname, FCP_CFG_IFNAME);
	if (p && p->fcoe_enable) {
		/* no action needed */
		rc = ENOACTION;
		goto out;
	}
	/* re-enable previous VLANs */
	if (p && p->auto_vlan) {
		vp = fcm_find_fcoe_port(p->ifname, FCP_REAL_IFNAME);
		while (vp) {
			if (vp->auto_created)
				vp->fcoe_enable = 1;
			vp = fcm_find_next_fcoe_port(vp, p->ifname);
		}
	}

	/*
	 * This looks odd, and could use some improvement. We may
	 * or may not have found a valid port. fcm_port_create
	 * will execute the 'cmd' even if it doesn't allocate a
	 * new port. fcm_port_create should probably be split
	 * into two routines, one that allocs a new port and one
	 * that executes the command.
	 */
	p = fcm_port_create(ifname, FCP_CREATE_IF);
	if (!p)
		goto out;

	rc = SUCCESS;

out:
	p->sock_reply = *r;
	return rc;
}

static enum fcoe_status fcm_cli_destroy(char *ifname,
					struct sock_info **r)
{
	struct fcoe_port *p;

	p = fcm_find_fcoe_port(ifname, FCP_CFG_IFNAME);
	if (p) {
		if (p->fcoe_enable) {
			p->fcoe_enable = 0;
			fcp_set_next_action(p, FCP_DESTROY_IF);
			p->sock_reply = *r;
			return SUCCESS;
		} else {
			/* no action needed */
			return ENOACTION;
		}
	}

	FCM_LOG_ERR(errno, "%s is not in port list.\n", ifname);
	return EFAIL;
}

static enum fcoe_status fcm_cli_action(char *ifname, int cmd,
				       struct sock_info **r)
{
	struct fcoe_port *p;

	p = fcm_find_fcoe_port(ifname, FCP_CFG_IFNAME);
	if (p) {
		fcp_set_next_action(p, cmd);
		p->sock_reply = *r;
		return SUCCESS;
	}

	FCM_LOG_ERR(errno, "%s is not in port list.\n", ifname);
	return EFAIL;
}

static struct sock_info *fcm_alloc_reply(struct sockaddr_un *f,
					 socklen_t flen, int s)
{
	static struct sock_info *r;

	if (flen > sizeof(*f))
		return NULL;

	r = (struct sock_info *)malloc(sizeof(struct sock_info));
	if (!r) {
		FCM_LOG_ERR(errno, "Failed in alloc reply sock info.\n");
		return NULL;
	}
	r->sock = s;
	memcpy(&r->from, f, flen);
	r->fromlen = flen;
	return r;
}

/*
 * receive function registered in sa_select_loop
 */
static void fcm_srv_receive(void *arg)
{
	struct fcm_srv_info *srv_info = arg;
	struct clif_data *data;
	struct sockaddr_un from;
	socklen_t fromlen = sizeof(struct sockaddr_un);
	struct sock_info *reply = NULL;
	char buf[MAX_MSGBUF], rbuf[MAX_MSGBUF];
	char *ifname;
	enum fcoe_status rc = EFAIL;
	int res, cmd, snum;

	snum = srv_info->srv_sock;
	res = recvfrom(snum, buf, sizeof(buf) - 1,
		       MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen);
	if (res < 0) {
		FCM_LOG_ERR(errno, "Fail in fcm_srv_receive()");
		return;
	}

	buf[res] = '\0';
	data = (struct clif_data *)buf;

	cmd = data->cmd;
	ifname = strdup(data->ifname);

	rc = fcoe_validate_interface(ifname);
	if (rc)
		goto err;

	reply = fcm_alloc_reply(&from, fromlen, snum);
	if (!reply)
		goto err_out;

	switch (cmd) {
	case CLIF_CREATE_CMD:
		FCM_LOG_DBG("FCMON CREATE\n");
		rc = fcm_cli_create(ifname, &reply);
		if (rc)
			goto err_out;
		break;
	case CLIF_DESTROY_CMD:
		FCM_LOG_DBG("FCMON DESTROY\n");
		rc = fcm_cli_destroy(ifname, &reply);
		if (rc)
			goto err_out;
		break;
	case CLIF_RESET_CMD:
		FCM_LOG_DBG("FCMON RESET\n");
		rc = fcm_cli_action(ifname, FCP_RESET_IF, &reply);
		if (rc)
			goto err_out;
		break;
	case CLIF_SCAN_CMD:
		FCM_LOG_DBG("FCMON SCAN\n");
		rc = fcm_cli_action(ifname, FCP_SCAN_IF, &reply);
		if (rc)
			goto err_out;
		break;
	default:
		FCM_LOG_DBG("FCMON INVALID CMD\n");
		goto err_out;
	}

	free(ifname);
	return;

err_out:
	free(ifname);
	free(reply);
err:
	snprintf(rbuf, MSG_RBUF, "%d", rc);
	sendto(snum, rbuf, MSG_RBUF, 0, (struct sockaddr *)&from, fromlen);
}

static int fcm_srv_create(struct fcm_srv_info *srv_info)
{
	struct sockaddr_un addr;
	int rc = 0;

	if (mkdir(FCM_SRV_DIR, S_IRWXU | S_IRWXG) < 0) {
		if (errno == EEXIST) {
			FCM_LOG_ERR(errno, "Failed to create socket "
				    "directory %s, this indicates that "
				    "fcoemon was not shutdown cleanly",
				    FCM_SRV_DIR);
		} else {
			rc = errno;
			FCM_LOG_ERR(errno, "Failed to create socket "
				    "directory %s\n", FCM_SRV_DIR);
			goto err;
		}
	}

	srv_info->srv_sock = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (srv_info->srv_sock < 0) {
		FCM_LOG_ERR(errno, "Failed to create socket\n");
		rc = errno;
		goto err_rmdir;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CLIF_SOCK_FILE, sizeof(addr.sun_path));

	/*
	 * If there was a previous socket file unlink. If we don't
	 * then bind will fail.
	 */
	unlink(CLIF_SOCK_FILE);

	if (bind(srv_info->srv_sock, (struct sockaddr *)&addr,
		 sizeof(addr)) < 0) {
		FCM_LOG_ERR(errno, "Failed to bind socket\n");
		rc = errno;
		goto err_close;
	}

	sa_select_add_fd(srv_info->srv_sock, fcm_srv_receive,
			 NULL, NULL, srv_info);

	FCM_LOG_DBG("Successfully created socket, socket file and binding\n");

	return rc;

err_close:
	close(srv_info->srv_sock);
	unlink(CLIF_SOCK_FILE);

err_rmdir:
	rmdir(FCM_SRV_DIR);

err:
	return rc;
}

static void fcm_srv_destroy(struct fcm_srv_info *srv_info)
{
	FCM_LOG_DBG("Shutdown fcmon server");
	close(srv_info->srv_sock);
	unlink(CLIF_SOCK_FILE);
	rmdir(FCM_SRV_DIR);
}

int main(int argc, char **argv)
{
	struct fcm_srv_info srv_info;
	struct sigaction sig;
	int fcm_fg = 0;
	int rc;
	int c;

	memset(&fcoe_config, 0, sizeof(fcoe_config));

	strncpy(progname, basename(argv[0]), sizeof(progname));
	sa_log_prefix = progname;
	sa_log_flags = 0;
	openlog(sa_log_prefix, LOG_CONS, LOG_DAEMON);

	while ((c = getopt_long(argc, argv, "fdhsv",
				fcm_options, NULL)) != -1) {
		switch (c) {
		case 'f':
			fcm_fg = 1;
			break;
		case 'd':
			fcoe_config.debug = 1;
			enable_debug_log(1);
			break;
		case 's':
			fcoe_config.use_syslog = 1;
			enable_syslog(1);
			break;
		case 'v':
			printf("%s\n", FCOE_UTILS_VERSION);
			return 0;
		case 'h':
		default:
			fcm_usage();
		break;
		}
	}
	if (argc != optind)
		fcm_usage();

	if (!fcm_fg && daemon(0, !fcoe_config.use_syslog)) {
		FCM_LOG("Starting daemon failed");
		exit(EXIT_FAILURE);
	}

	umask(0);

	/*
	 * Set up for signals.
	 */
	memset(&sig, 0, sizeof(sig));
	sig.sa_handler = fcm_sig;

	rc = sigaction(SIGINT, &sig, NULL);
	if (rc < 0) {
		FCM_LOG_ERR(errno, "Failed to register handler for SIGINT");
		exit(1);
	}
	rc = sigaction(SIGTERM, &sig, NULL);
	if (rc < 0) {
		FCM_LOG_ERR(errno, "Failed to register handler for SIGTERM");
		exit(1);
	}
	rc = sigaction(SIGHUP, &sig, NULL);
	if (rc < 0) {
		FCM_LOG_ERR(errno, "Failed to register handler for SIGHUP");
		exit(1);
	}
	fcm_pidfile_create();

	/* check fcoe module */
	if (fcoe_checkdir(SYSFS_FCOE)) {
		FCM_LOG_ERR(errno, "make sure FCoE driver module is loaded!");
		exit(1);
	}

	fcm_fcoe_init();
	fcm_fc_events_init();
	fcm_link_init();	/* NETLINK_ROUTE protocol */
	fcm_dcbd_init();
	fcm_srv_create(&srv_info);
	sa_select_set_callback(fcm_handle_changes);

	rc = sa_select_loop();
	if (rc < 0) {
		FCM_LOG_ERR(rc, "select error\n");
		exit(EXIT_FAILURE);
	}
	fcm_dcbd_shutdown();
	fcm_srv_destroy(&srv_info);
	if (rc == SIGHUP)
		fcm_cleanup();
	return 0;
}

/*******************************************************
 *         The following are debug routines            *
 *******************************************************/
static void add_msg_to_buf(char *buf, int maxlen, char *msg, char *prefix)
{
	int len = strlen(buf);

	if (len + strlen(msg) + strlen(prefix) < maxlen)
		sprintf(buf+len, "%s%s", prefix, msg);
}

static void print_errors(int errors)
{
	char msg[256];
	int cnt = 0;

	memset(msg, 0, sizeof(msg));
	sprintf(msg, "0x%02x - ", errors);

	if (errors & 0x01)
		add_msg_to_buf(msg, sizeof(msg), "mismatch with peer",
			       (cnt++) ? ", " : "");

	if (errors & 0x02)
		add_msg_to_buf(msg, sizeof(msg), "local configuration error",
			       (cnt++) ? ", " : "");

	if (errors & 0x04)
		add_msg_to_buf(msg, sizeof(msg), "multiple TLV's received",
			       (cnt++) ? ", " : "");

	if (errors & 0x08)
		add_msg_to_buf(msg, sizeof(msg), "peer error",
			       (cnt++) ? ", " : "");

	if (errors & 0x10)
		add_msg_to_buf(msg, sizeof(msg), "multiple LLDP neighbors",
			       (cnt++) ? ", " : "");

	if (errors & 0x20)
		add_msg_to_buf(msg, sizeof(msg), "peer feature not present",
			       (cnt++) ? ", " : "");

	if (!errors)
		add_msg_to_buf(msg, sizeof(msg), "none", "");

	FCM_LOG("%s\n", msg);
}
