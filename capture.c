/* capture.c
 * Routines for packet capture windows
 *
 * $Id: capture.c,v 1.63 1999/09/01 03:04:09 gram Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@zing.org>
 * Copyright 1998 Gerald Combs
 *
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */


#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_LIBPCAP

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <time.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#ifdef HAVE_NET_IF_H
#include <net/if.h>
#endif

#include <signal.h>
#include <errno.h>

#ifdef NEED_SNPRINTF_H
# ifdef HAVE_STDARG_H
#  include <stdarg.h>
# else
#  include <varargs.h>
# endif
# include "snprintf.h"
#endif

#ifdef HAVE_SYS_SOCKIO_H
# include <sys/sockio.h>
#endif

#include "ethereal.h"
#include "packet.h"
#include "file.h"
#include "gtk/menu.h"
#include "capture.h"
#include "util.h"
#include "prefs.h"

extern capture_file  cf;
extern GtkWidget    *info_bar;
extern guint         file_ctx;

extern gchar *ethereal_path;
extern gchar *medium_font;
extern gchar *bold_font;
extern int fork_mode;
extern int sync_pipe[];
extern int sync_mode;
extern int sigusr2_received;
extern int quit_after_cap;

/* Capture callback data keys */
#define E_CAP_IFACE_KEY "cap_iface"
#define E_CAP_FILT_KEY  "cap_filter"
#define E_CAP_COUNT_KEY "cap_count"
#define E_CAP_OPEN_KEY  "cap_open"
#define E_CAP_SNAP_KEY  "cap_snap"

/* Capture filter key */
#define E_CAP_FILT_TE_KEY "cap_filt_te"

static void search_for_if_cb(gpointer data, gpointer user_data);
static void free_if_cb(gpointer data, gpointer user_data);
static void capture_prep_ok_cb(GtkWidget *, gpointer);
static void capture_prep_close_cb(GtkWidget *, gpointer);
static float pct(gint, gint);
static void capture_stop_cb(GtkWidget *, gpointer);
static void capture_pcap_cb(u_char *, const struct pcap_pkthdr *,
  const u_char *);

struct search_user_data {
	char	*name;
	int	found;
};

static GList *
get_interface_list() {
  GList  *il = NULL;
  gint    nonloopback_pos = 0;
  struct  ifreq *ifr, *last;
  struct  ifconf ifc;
  struct  ifreq ifrflags;
  int     sock = socket(AF_INET, SOCK_DGRAM, 0);
  struct search_user_data user_data;

  if (sock < 0)
  {
    simple_dialog(ESD_TYPE_WARN, NULL,
      "Can't list interfaces: error opening socket.");
    return NULL;
  }

  /* Since we have to grab the interface list all at once, we'll make
     plenty of room */
  ifc.ifc_len = 1024 * sizeof(struct ifreq);
  ifc.ifc_buf = malloc(ifc.ifc_len);

  if (ioctl(sock, SIOCGIFCONF, &ifc) < 0 ||
    ifc.ifc_len < sizeof(struct ifreq))
  {
    simple_dialog(ESD_TYPE_WARN, NULL,
      "Can't list interfaces: SIOCGIFCONF error: %s", strerror(errno));
    return NULL;
  }

  ifr  = (struct ifreq *) ifc.ifc_req;
  last = (struct ifreq *) ((char *) ifr + ifc.ifc_len);
  while (ifr < last)
  {
    /*
     * Skip addresses that begin with "dummy", or that include a ":"
     * (the latter are Solaris virtuals).
     */
    if (strncmp(ifr->ifr_name, "dummy", 5) == 0 ||
	strchr(ifr->ifr_name, ':') != NULL)
      goto next;

    /*
     * If we already have this interface name on the list, don't add
     * it (SIOCGIFCONF returns, at least on BSD-flavored systems, one
     * entry per interface *address*; if an interface has multiple
     * addresses, we get multiple entries for it).
     */
    user_data.name = ifr->ifr_name;
    user_data.found = FALSE;
    g_list_foreach(il, search_for_if_cb, &user_data);
    if (user_data.found)
      goto next;

    /*
     * Get the interface flags.
     */
    memset(&ifrflags, 0, sizeof ifrflags);
    strncpy(ifrflags.ifr_name, ifr->ifr_name, sizeof ifrflags.ifr_name);
    if (ioctl(sock, SIOCGIFFLAGS, (char *)&ifrflags) < 0) {
      if (errno == ENXIO)
        goto next;
      simple_dialog(ESD_TYPE_WARN, NULL,
        "Can't list interfaces: SIOCGIFFLAGS error on %s: %s",
        ifr->ifr_name, strerror(errno));
      goto fail;
    }

    /*
     * Skip interfaces that aren't up.
     */
    if (!(ifrflags.ifr_flags & IFF_UP))
      goto next;

    /*
     * If it's a loopback interface, add it at the end of the list,
     * otherwise add it after the last non-loopback interface,
     * so all loopback interfaces go at the end - we don't want a
     * loopback interface to be the default capture device unless there
     * are no non-loopback devices.
     */
    if ((ifrflags.ifr_flags & IFF_LOOPBACK) ||
	strncmp(ifr->ifr_name, "lo", 2) == 0)
      il = g_list_insert(il, g_strdup(ifr->ifr_name), -1);
    else {
      il = g_list_insert(il, g_strdup(ifr->ifr_name), nonloopback_pos);
      /* Insert the next non-loopback interface after this one. */
      nonloopback_pos++;
    }

next:
#ifdef HAVE_SA_LEN
    ifr = (struct ifreq *) ((char *) ifr + ifr->ifr_addr.sa_len + IFNAMSIZ);
#else
    ifr = (struct ifreq *) ((char *) ifr + sizeof(struct ifreq));
#endif
  }

  free(ifc.ifc_buf);
  return il;

fail:
  g_list_foreach(il, free_if_cb, NULL);
  g_list_free(il);
  return NULL;
}

static void
search_for_if_cb(gpointer data, gpointer user_data)
{
	struct search_user_data *search_user_data = user_data;

	if (strcmp((char *)data, search_user_data->name) == 0)
		search_user_data->found = TRUE;
}

static void
free_if_cb(gpointer data, gpointer user_data)
{
	g_free(data);
}

void
capture_prep_cb(GtkWidget *w, gpointer d) {
  GtkWidget     *cap_open_w, *if_cb, *if_lb,
                *count_lb, *count_cb, *main_vb, *if_hb, *count_hb,
                *filter_hb, *filter_bt, *filter_te, *caplen_hb,
                *bbox, *ok_bt, *cancel_bt, *snap_lb,
                *snap_sb;
  GtkAdjustment *adj;
  GList         *if_list, *count_list = NULL;
  gchar         *count_item1 = "0 (Infinite)", count_item2[16];

  cap_open_w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(cap_open_w), "Ethereal: Capture Preferences");
  
  /* Container for each row of widgets */
  main_vb = gtk_vbox_new(FALSE, 3);
  gtk_container_border_width(GTK_CONTAINER(main_vb), 5);
  gtk_container_add(GTK_CONTAINER(cap_open_w), main_vb);
  gtk_widget_show(main_vb);
  
  /* Interface row */
  if_hb = gtk_hbox_new(FALSE, 3);
  gtk_container_add(GTK_CONTAINER(main_vb), if_hb);
  gtk_widget_show(if_hb);
  
  if_lb = gtk_label_new("Interface:");
  gtk_box_pack_start(GTK_BOX(if_hb), if_lb, FALSE, FALSE, 0);
  gtk_widget_show(if_lb);
  
  if_list = get_interface_list();
  
  if_cb = gtk_combo_new();
  gtk_combo_set_popdown_strings(GTK_COMBO(if_cb), if_list);
  if (cf.iface)
    gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(if_cb)->entry), cf.iface);
  else if (if_list)
    gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(if_cb)->entry), if_list->data);
  gtk_box_pack_start(GTK_BOX(if_hb), if_cb, FALSE, FALSE, 0);
  gtk_widget_show(if_cb);
  
  while (if_list) {
    g_free(if_list->data);
    if_list = g_list_remove_link(if_list, if_list);
  }

  /* Count row */
  count_hb = gtk_hbox_new(FALSE, 3);
  gtk_container_add(GTK_CONTAINER(main_vb), count_hb);
  gtk_widget_show(count_hb);
  
  count_lb = gtk_label_new("Count:");
  gtk_box_pack_start(GTK_BOX(count_hb), count_lb, FALSE, FALSE, 0);
  gtk_widget_show(count_lb);
  
  count_list = g_list_append(count_list, count_item1);
  if (cf.count) {
    snprintf(count_item2, 15, "%d", cf.count);
    count_list = g_list_append(count_list, count_item2);
  }

  count_cb = gtk_combo_new();
  gtk_combo_set_popdown_strings(GTK_COMBO(count_cb), count_list);
  gtk_box_pack_start(GTK_BOX(count_hb), count_cb, FALSE, FALSE, 0);
  gtk_widget_show(count_cb);

  while (count_list)
    count_list = g_list_remove_link(count_list, count_list);

  /* Filter row */
  filter_hb = gtk_hbox_new(FALSE, 3);
  gtk_container_add(GTK_CONTAINER(main_vb), filter_hb);
  gtk_widget_show(filter_hb);
  
  filter_bt = gtk_button_new_with_label("Filter:");
  gtk_signal_connect(GTK_OBJECT(filter_bt), "clicked",
    GTK_SIGNAL_FUNC(prefs_cb), (gpointer) E_PR_PG_FILTER);
  gtk_box_pack_start(GTK_BOX(filter_hb), filter_bt, FALSE, TRUE, 0);
  gtk_widget_show(filter_bt);
  
  filter_te = gtk_entry_new();
  if (cf.cfilter) gtk_entry_set_text(GTK_ENTRY(filter_te), cf.cfilter);
  gtk_object_set_data(GTK_OBJECT(filter_bt), E_FILT_TE_PTR_KEY, filter_te);
  gtk_box_pack_start(GTK_BOX(filter_hb), filter_te, TRUE, TRUE, 0);
  gtk_widget_show(filter_te);

  /* Misc row: Capture file checkbox and snap spinbutton */
  caplen_hb = gtk_hbox_new(FALSE, 3);
  gtk_container_add(GTK_CONTAINER(main_vb), caplen_hb);
  gtk_widget_show(caplen_hb);

  snap_lb = gtk_label_new("Capture length");
  gtk_misc_set_alignment(GTK_MISC(snap_lb), 0, 0.5);
  gtk_box_pack_start(GTK_BOX(caplen_hb), snap_lb, FALSE, FALSE, 6);
  gtk_widget_show(snap_lb);

  adj = (GtkAdjustment *) gtk_adjustment_new((float) cf.snap,
    MIN_PACKET_SIZE, WTAP_MAX_PACKET_SIZE, 1.0, 10.0, 0.0);
  snap_sb = gtk_spin_button_new (adj, 0, 0);
  gtk_spin_button_set_wrap (GTK_SPIN_BUTTON (snap_sb), TRUE);
  gtk_widget_set_usize (snap_sb, 80, 0);
  gtk_box_pack_start (GTK_BOX(caplen_hb), snap_sb, FALSE, FALSE, 3); 
  gtk_widget_show(snap_sb);
  
  /* Button row: OK and cancel buttons */
  bbox = gtk_hbutton_box_new();
  gtk_button_box_set_layout (GTK_BUTTON_BOX (bbox), GTK_BUTTONBOX_END);
  gtk_button_box_set_spacing(GTK_BUTTON_BOX(bbox), 5);
  gtk_container_add(GTK_CONTAINER(main_vb), bbox);
  gtk_widget_show(bbox);

  ok_bt = gtk_button_new_with_label ("OK");
  gtk_signal_connect(GTK_OBJECT(ok_bt), "clicked",
    GTK_SIGNAL_FUNC(capture_prep_ok_cb), GTK_OBJECT(cap_open_w));
  GTK_WIDGET_SET_FLAGS(ok_bt, GTK_CAN_DEFAULT);
  gtk_box_pack_start (GTK_BOX (bbox), ok_bt, TRUE, TRUE, 0);
  gtk_widget_grab_default(ok_bt);
  gtk_widget_show(ok_bt);

  cancel_bt = gtk_button_new_with_label ("Cancel");
  gtk_signal_connect(GTK_OBJECT(cancel_bt), "clicked",
    GTK_SIGNAL_FUNC(capture_prep_close_cb), GTK_OBJECT(cap_open_w));
  GTK_WIDGET_SET_FLAGS(cancel_bt, GTK_CAN_DEFAULT);
  gtk_box_pack_start (GTK_BOX (bbox), cancel_bt, TRUE, TRUE, 0);
  gtk_widget_show(cancel_bt);

  /* Attach pointers to needed widgets to the capture prefs window/object */
  gtk_object_set_data(GTK_OBJECT(cap_open_w), E_CAP_IFACE_KEY, if_cb);
  gtk_object_set_data(GTK_OBJECT(cap_open_w), E_CAP_FILT_KEY,  filter_te);
  gtk_object_set_data(GTK_OBJECT(cap_open_w), E_CAP_COUNT_KEY, count_cb);
  gtk_object_set_data(GTK_OBJECT(cap_open_w), E_CAP_SNAP_KEY,  snap_sb);

  gtk_widget_show(cap_open_w);
}

static void
capture_prep_ok_cb(GtkWidget *ok_bt, gpointer parent_w) {
  GtkWidget *if_cb, *filter_te, *count_cb, *snap_sb;
  gchar *filter_text;
  char tmpname[128+1];

  if_cb     = (GtkWidget *) gtk_object_get_data(GTK_OBJECT(parent_w), E_CAP_IFACE_KEY);
  filter_te = (GtkWidget *) gtk_object_get_data(GTK_OBJECT(parent_w), E_CAP_FILT_KEY);
  count_cb  = (GtkWidget *) gtk_object_get_data(GTK_OBJECT(parent_w), E_CAP_COUNT_KEY);
  snap_sb   = (GtkWidget *) gtk_object_get_data(GTK_OBJECT(parent_w), E_CAP_SNAP_KEY);

  if (cf.iface) g_free(cf.iface);
  cf.iface =
    g_strdup(gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(if_cb)->entry)));

  filter_text = gtk_entry_get_text(GTK_ENTRY(filter_te));
  if (cf.cfilter) g_free(cf.cfilter);
  cf.cfilter = NULL; /* ead 06/16/99 */
  if (filter_text && filter_text[0]) {
	  cf.cfilter = g_strdup(gtk_entry_get_text(GTK_ENTRY(filter_te))); 
  }
  cf.count = atoi(gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(count_cb)->entry)));
  cf.snap = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(snap_sb));
  if (cf.snap < 1)
    cf.snap = WTAP_MAX_PACKET_SIZE;
  else if (cf.snap < MIN_PACKET_SIZE)
    cf.snap = MIN_PACKET_SIZE;

  gtk_widget_destroy(GTK_WIDGET(parent_w));

  /* Choose a random name for the capture buffer */
  if (cf.save_file && !cf.user_saved) {
	unlink(cf.save_file); /* silently ignore error */
	g_free(cf.save_file);
  }
  cf.save_file_fd = create_tempfile(tmpname, sizeof tmpname, "ether");
  cf.save_file = g_strdup(tmpname);
  cf.user_saved = 0;
  
  if( fork_mode ){	/*  use fork() for capture */
    int  fork_child;
    char ssnap[24];
    char scount[24];	/* need a constant for len of numbers */
    char save_file_fd[24];
    int err;

    sprintf(ssnap,"%d",cf.snap); /* in lieu of itoa */
    sprintf(scount,"%d",cf.count);
    sprintf(save_file_fd,"%d",cf.save_file_fd);
    signal(SIGCHLD, SIG_IGN);
    if (sync_mode) pipe(sync_pipe);
    if((fork_child = fork()) == 0){
      /* args: -k -- capture
       * -i interface specification
       * -w file to write
       * -W file descriptor to write
       * -c count to capture
       * -Q quit after capture (forces -k)
       * -s snaplen
       * -S sync mode
       * -m / -b fonts
       * -f "filter expression"
       */
       if (sync_mode) {
	 close(1);
	 dup(sync_pipe[1]);
	 close(sync_pipe[0]);
	 execlp(ethereal_path, "ethereal", "-k", "-Q", "-i", cf.iface,
		"-w", cf.save_file, "-W", save_file_fd,
		"-c", scount, "-s", ssnap, "-S", 
		"-m", medium_font, "-b", bold_font,
		(cf.cfilter == NULL)? 0 : "-f",
		(cf.cfilter == NULL)? 0 : cf.cfilter,
		(const char *)NULL);	
       }
       else {
	 execlp(ethereal_path, "ethereal", "-k", "-Q", "-i", cf.iface,
		"-w", cf.save_file, "-W", save_file_fd,
		"-c", scount, "-s", ssnap,
		"-m", medium_font, "-b", bold_font,
		(cf.cfilter == NULL)? 0 : "-f",
		(cf.cfilter == NULL)? 0 : cf.cfilter,
		(const char *)NULL);
       }
    }
    else {
       cf.filename = cf.save_file;
       if (sync_mode) {
	 close(sync_pipe[1]);
	 while (!sigusr2_received) {
	   struct timeval timeout = {1,0};
	   select(0, NULL, NULL, NULL, &timeout);
	   if (kill(fork_child, 0) == -1 && errno == ESRCH) 
	     break;
	 }
	 if (sigusr2_received) {
	   err = tail_cap_file(cf.save_file, &cf);
	   if (err != 0) {
	     simple_dialog(ESD_TYPE_WARN, NULL,
			file_open_error_message(err, FALSE), cf.save_file);
	   }
	 }
	 sigusr2_received = FALSE;
       }
    }
  }
  else
    capture();
}

static void
capture_prep_close_cb(GtkWidget *close_bt, gpointer parent_w)
{
  gtk_grab_remove(GTK_WIDGET(parent_w));
  gtk_widget_destroy(GTK_WIDGET(parent_w));
}

typedef struct _loop_data {
  gint           go;
  gint           max;
  gint           linktype;
  gint           sync_packets;
  packet_counts  counts;
  wtap_dumper   *pdh;
} loop_data;

void
capture(void) {
  GtkWidget  *cap_w, *main_vb, *count_lb, *tcp_lb, *udp_lb, *icmp_lb,
             *ospf_lb, *gre_lb, *netbios_lb, *other_lb, *stop_bt;
  pcap_t     *pch;
  gchar       err_str[PCAP_ERRBUF_SIZE], label_str[32];
  loop_data   ld;
  bpf_u_int32 netnum, netmask;
  time_t      upd_time, cur_time;
  int         err, inpkts;
  char       *errmsg;
  char        errmsg_errno[1024+1];

  ld.go             = TRUE;
  ld.counts.total   = 0;
  ld.max            = cf.count;
  ld.linktype       = WTAP_ENCAP_UNKNOWN;
  ld.sync_packets   = 0;
  ld.counts.tcp     = 0;
  ld.counts.udp     = 0;
  ld.counts.icmp    = 0;
  ld.counts.ospf    = 0;
  ld.counts.gre     = 0;
  ld.counts.netbios = 0;
  ld.counts.other   = 0;
  ld.pdh            = NULL;

  close_cap_file(&cf, info_bar, file_ctx);

  pch = pcap_open_live(cf.iface, cf.snap, 1, 250, err_str);

  if (pch) {
    ld.linktype = wtap_pcap_encap_to_wtap_encap(pcap_datalink(pch));
    if (ld.linktype == WTAP_ENCAP_UNKNOWN) {
      errmsg = "The network you're capturing from is of a type"
               " that Ethereal doesn't support.";
      goto fail;
    }
    ld.pdh = wtap_dump_fdopen(cf.save_file_fd, WTAP_FILE_PCAP,
		ld.linktype, pcap_snapshot(pch), &err);

    if (ld.pdh == NULL) {  /* We have an error */
      switch (err) {

      case WTAP_ERR_CANT_OPEN:
        errmsg = "The file to which the capture would be saved"
                 " couldn't be created for some unknown reason.";
        break;

      case WTAP_ERR_SHORT_WRITE:
        errmsg = "A full header couldn't be written to the file"
                 " to which the capture would be saved.";
        break;

      default:
        if (err < 0) {
          sprintf(errmsg_errno, "The file to which the capture would be"
	                      " saved (\"%%s\") could not be opened: Error %d.",
	  			err);
        } else {
          sprintf(errmsg_errno, "The file to which the capture would be"
	                      " saved (\"%%s\") could not be opened: %s.",
	  			strerror(err));
	}
	errmsg = errmsg_errno;
	break;
      }
fail:
      snprintf(err_str, PCAP_ERRBUF_SIZE, errmsg, cf.save_file);
      simple_dialog(ESD_TYPE_WARN, NULL, err_str);
      pcap_close(pch);
      return;
    }

    if (cf.cfilter) {
      if (pcap_lookupnet (cf.iface, &netnum, &netmask, err_str) < 0) {
        simple_dialog(ESD_TYPE_WARN, NULL,
          "Can't use filter:  Couldn't obtain netmask info (%s).", err_str);
        wtap_dump_close(ld.pdh, NULL);
        unlink(cf.save_file); /* silently ignore error */
        pcap_close(pch);
        return;
      } else if (pcap_compile(pch, &cf.fcode, cf.cfilter, 1, netmask) < 0) {
        simple_dialog(ESD_TYPE_WARN, NULL, "Unable to parse filter string (%s).",
			pcap_geterr(pch));
        wtap_dump_close(ld.pdh, NULL);
        unlink(cf.save_file); /* silently ignore error */
        pcap_close(pch);
        return;
      } else if (pcap_setfilter(pch, &cf.fcode) < 0) {
        simple_dialog(ESD_TYPE_WARN, NULL, "Can't install filter (%s).",
			pcap_geterr(pch));
        wtap_dump_close(ld.pdh, NULL);
        unlink(cf.save_file); /* silently ignore error */
        pcap_close(pch);
        return;
      }
    }

    if (sync_mode) {
      /* Sync out the capture file, so the header makes it to the file
         system, and signal our parent so that they'll open the capture
	 file and update its windows to indicate that we have a live
	 capture in progress. */
      fflush(wtap_dump_file(ld.pdh));
      kill(getppid(), SIGUSR2);
    }

    cap_w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(cap_w), "Ethereal: Capture / Playback");

    /* Container for capture display widgets */
    main_vb = gtk_vbox_new(FALSE, 1);
    gtk_container_border_width(GTK_CONTAINER(main_vb), 5);
    gtk_container_add(GTK_CONTAINER(cap_w), main_vb);
    gtk_widget_show(main_vb);

    count_lb = gtk_label_new("Count: 0");
    gtk_box_pack_start(GTK_BOX(main_vb), count_lb, FALSE, FALSE, 3);
    gtk_widget_show(count_lb);

    tcp_lb = gtk_label_new("TCP: 0 (0.0%)");
    gtk_box_pack_start(GTK_BOX(main_vb), tcp_lb, FALSE, FALSE, 3);
    gtk_widget_show(tcp_lb);

    udp_lb = gtk_label_new("UDP: 0 (0.0%)");
    gtk_box_pack_start(GTK_BOX(main_vb), udp_lb, FALSE, FALSE, 3);
    gtk_widget_show(udp_lb);

    icmp_lb = gtk_label_new("ICMP: 0 (0.0%)");
    gtk_box_pack_start(GTK_BOX(main_vb), icmp_lb, FALSE, FALSE, 3);
    gtk_widget_show(icmp_lb);

    ospf_lb = gtk_label_new("OSPF: 0 (0.0%)");
    gtk_box_pack_start(GTK_BOX(main_vb), ospf_lb, FALSE, FALSE, 3);
    gtk_widget_show(ospf_lb);

    gre_lb = gtk_label_new("GRE: 0 (0.0%)");
    gtk_box_pack_start(GTK_BOX(main_vb), gre_lb, FALSE, FALSE, 3);
    gtk_widget_show(gre_lb);

    netbios_lb = gtk_label_new("NetBIOS: 0 (0.0%)");
    gtk_box_pack_start(GTK_BOX(main_vb), netbios_lb, FALSE, FALSE, 3);
    gtk_widget_show(netbios_lb);

    other_lb = gtk_label_new("Other: 0 (0.0%)");
    gtk_box_pack_start(GTK_BOX(main_vb), other_lb, FALSE, FALSE, 3);
    gtk_widget_show(other_lb);

    stop_bt = gtk_button_new_with_label ("Stop");
    gtk_signal_connect(GTK_OBJECT(stop_bt), "clicked",
      GTK_SIGNAL_FUNC(capture_stop_cb), (gpointer) &ld);
    gtk_box_pack_end(GTK_BOX(main_vb), stop_bt, FALSE, FALSE, 3);
    GTK_WIDGET_SET_FLAGS(stop_bt, GTK_CAN_DEFAULT);
    gtk_widget_grab_default(stop_bt);
    GTK_WIDGET_SET_FLAGS(stop_bt, GTK_CAN_DEFAULT);
    gtk_widget_grab_default(stop_bt);
    gtk_widget_show(stop_bt);

    gtk_widget_show(cap_w);
    gtk_grab_add(cap_w);

    upd_time = time(NULL);
    while (ld.go) {
      while (gtk_events_pending()) gtk_main_iteration();
      inpkts = pcap_dispatch(pch, 1, capture_pcap_cb, (u_char *) &ld);
      if (inpkts > 0)
        ld.sync_packets += inpkts;
      /* Only update once a second so as not to overload slow displays */
      cur_time = time(NULL);
      if (cur_time > upd_time) {

        upd_time = cur_time;

        sprintf(label_str, "Count: %d", ld.counts.total);
        gtk_label_set(GTK_LABEL(count_lb), label_str);

        sprintf(label_str, "TCP: %d (%.1f%%)", ld.counts.tcp,
	   pct(ld.counts.tcp, ld.counts.total));
        gtk_label_set(GTK_LABEL(tcp_lb), label_str);

        sprintf(label_str, "UDP: %d (%.1f%%)", ld.counts.udp,
	  pct(ld.counts.udp, ld.counts.total));
        gtk_label_set(GTK_LABEL(udp_lb), label_str);

        sprintf(label_str, "ICMP: %d (%.1f%%)", ld.counts.icmp,
	  pct(ld.counts.icmp, ld.counts.total));
        gtk_label_set(GTK_LABEL(icmp_lb), label_str);

        sprintf(label_str, "OSPF: %d (%.1f%%)", ld.counts.ospf,
	  pct(ld.counts.ospf, ld.counts.total));
        gtk_label_set(GTK_LABEL(ospf_lb), label_str);

        sprintf(label_str, "GRE: %d (%.1f%%)", ld.counts.gre,
	  pct(ld.counts.gre, ld.counts.total));
        gtk_label_set(GTK_LABEL(gre_lb), label_str);

        sprintf(label_str, "NetBIOS: %d (%.1f%%)", ld.counts.netbios,
	  pct(ld.counts.netbios, ld.counts.total));
        gtk_label_set(GTK_LABEL(netbios_lb), label_str);

        sprintf(label_str, "Other: %d (%.1f%%)", ld.counts.other,
          pct(ld.counts.other, ld.counts.total));
        gtk_label_set(GTK_LABEL(other_lb), label_str);

	/* do sync here, too */
	fflush(wtap_dump_file(ld.pdh));
	if (sync_mode && ld.sync_packets) {
	  char tmp[20];
	  sprintf(tmp, "%d*", ld.sync_packets);
	  write(1, tmp, strlen(tmp));
	  ld.sync_packets = 0;
	}
      }
    }
    
    if (ld.pdh) {
      if (!wtap_dump_close(ld.pdh, &err)) {
        switch (err) {

        case WTAP_ERR_CANT_CLOSE:
          errmsg = "The file to which the capture was being saved"
                 " couldn't be closed for some unknown reason.";
          break;

        case WTAP_ERR_SHORT_WRITE:
          errmsg = "Not all the data could be written to the file"
                   " to which the capture was being saved.";
          break;

        default:
          if (err < 0) {
            sprintf(errmsg_errno, "The file to which the capture was being"
	                      " saved (\"%%s\") could not be closed: Error %d.",
	  			err);
          } else {
            sprintf(errmsg_errno, "The file to which the capture was being"
	                      " saved (\"%%s\") could not be closed: %s.",
	  			strerror(err));
	  }
	  errmsg = errmsg_errno;
	  break;
        }
        snprintf(err_str, PCAP_ERRBUF_SIZE, errmsg, cf.save_file);
        simple_dialog(ESD_TYPE_WARN, NULL, err_str);
      }
    }
    pcap_close(pch);

    gtk_grab_remove(GTK_WIDGET(cap_w));
    gtk_widget_destroy(GTK_WIDGET(cap_w));
  } else {
    while (gtk_events_pending()) gtk_main_iteration();
    simple_dialog(ESD_TYPE_WARN, NULL,
      "The capture session could not be initiated (%s).\n"
      "Please check to make sure you have sufficient permissions, and that\n"
      "you have the proper interface specified.", err_str);
  }

  if( quit_after_cap ){
    /* DON'T unlink the save file.  Presumably someone wants it. */
    gtk_exit(0);
  }

  if (pch) {
    /* "pch" is non-NULL only if we successfully started a capture.
       If we haven't, there's no capture file to load. */
    if ((err = open_cap_file(cf.save_file, &cf)) == 0) {
      /* Set the read filter to NULL. */
      cf.rfcode = NULL;
      err = read_cap_file(&cf);
      set_menu_sensitivity("/File/Save", TRUE);
      set_menu_sensitivity("/File/Save As...", FALSE);
    }
  }
}

static float
pct(gint num, gint denom) {
  if (denom) {
    return (float) num * 100.0 / (float) denom;
  } else {
    return 0.0;
  }
}

static void
capture_stop_cb(GtkWidget *w, gpointer data) {
  loop_data *ld = (loop_data *) data;
  
  ld->go = FALSE;
}

static void
capture_pcap_cb(u_char *user, const struct pcap_pkthdr *phdr,
  const u_char *pd) {
  struct wtap_pkthdr whdr;
  loop_data *ld = (loop_data *) user;
  int err;

  if ((++ld->counts.total >= ld->max) && (ld->max > 0)) 
  {
     ld->go = FALSE;
  }
  if (ld->pdh) {
     whdr.ts = phdr->ts;
     whdr.caplen = phdr->caplen;
     whdr.len = phdr->len;
     whdr.pkt_encap = ld->linktype;

     /* XXX - do something if this fails */
     wtap_dump(ld->pdh, &whdr, pd, &err);
  }
    
  switch (ld->linktype) {
    case WTAP_ENCAP_ETHERNET:
      capture_eth(pd, phdr->caplen, &ld->counts);
      break;
    case WTAP_ENCAP_FDDI:
    case WTAP_ENCAP_FDDI_BITSWAPPED:
      capture_fddi(pd, phdr->caplen, &ld->counts);
      break;
    case WTAP_ENCAP_TR:
      capture_tr(pd, phdr->caplen, &ld->counts);
      break;
    case WTAP_ENCAP_NULL:
      capture_null(pd, phdr->caplen, &ld->counts);
      break;
    case WTAP_ENCAP_PPP:
      capture_ppp(pd, phdr->caplen, &ld->counts);
      break;
    case WTAP_ENCAP_RAW_IP:
      capture_raw(pd, phdr->caplen, &ld->counts);
      break;
    /* XXX - FreeBSD may append 4-byte ATM pseudo-header to DLT_ATM_RFC1483,
       with LLC header following; we should implement it at some
       point. */
  }
}

#endif /* HAVE_LIBPCAP */
