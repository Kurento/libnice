/*
 * interfaces.c - Source for interface discovery code
 *
 * Copyright (C) 2006 Youness Alaoui <kakaroto@kakaroto.homelinux.net>
 * Copyright (C) 2007 Collabora, Nokia
 *  Contact: Youness Alaoui
 * Copyright (C) 2008 Haakon Sporsheim <haakon.sporsheim@tandberg.com>
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Dafydd Harries, Collabora Ltd.
 *   Youness Alaoui, Collabora Ltd.
 *   Kai Vehmanen, Nokia
 *   Philip Withnall, Collabora Ltd.
 *   Haakon Sporsheim
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "interfaces.h"
#include "agent-priv.h"

#ifdef G_OS_UNIX

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#ifdef __sun
#include <sys/sockio.h>
#endif

#ifdef HAVE_GETIFADDRS
 #include <ifaddrs.h>
#endif

#include <net/if.h>
#include <arpa/inet.h>

#endif /* G_OS_UNIX */

#ifdef IGNORED_IFACE_PREFIX
#ifdef G_OS_WIN32
#include <stdio.h>
#endif

static const gchar *ignored_iface_prefix_list[] = {
  IGNORED_IFACE_PREFIX,
  NULL
};
#endif

#if (defined(G_OS_UNIX) && defined(HAVE_GETIFADDRS)) || defined(G_OS_WIN32)
/* Works on both UNIX and Windows. Magic! */
static gchar *
sockaddr_to_string (const struct sockaddr *addr)
{
  char addr_as_string[INET6_ADDRSTRLEN+1];
  size_t addr_len;

  switch (addr->sa_family) {
    case AF_INET:
      addr_len = sizeof (struct sockaddr_in);
      break;
    case AF_INET6:
      addr_len = sizeof (struct sockaddr_in6);
      break;
    default:
      nice_debug ("Unknown sockaddr family: %i", addr->sa_family);
      return NULL;
  }

  if (getnameinfo (addr, addr_len,
          addr_as_string, sizeof (addr_as_string), NULL, 0,
          NI_NUMERICHOST) != 0) {
#ifdef G_OS_WIN32
    gchar *msg = g_win32_error_message (WSAGetLastError ());
    nice_debug ("Error running getnameinfo: %s", msg);
    g_free (msg);
#endif
    return NULL;
  }

  return g_strdup (addr_as_string);
}
#endif

#ifdef G_OS_UNIX

static GList *
get_local_interfaces_ioctl (void)
{
  GList *interfaces = NULL;
  gint sockfd;
  gint size = 0;
  struct ifreq *ifr;
  struct ifconf ifc;

  if ((sockfd = socket (AF_INET, SOCK_DGRAM, IPPROTO_IP)) < 0) {
    nice_debug ("error : Cannot open socket to retrieve interface list");
    return NULL;
  }

  ifc.ifc_len = 0;
  ifc.ifc_req = NULL;

  /* Loop and get each interface the system has, one by one... */
  do {
    size += sizeof (struct ifreq);
    /* realloc buffer size until no overflow occurs  */
    if (NULL == (ifc.ifc_req = realloc (ifc.ifc_req, size))) {
      nice_debug ("Error : Out of memory while allocation interface"
          "configuration structure");
      close (sockfd);
      return NULL;
    }
    ifc.ifc_len = size;

    if (ioctl (sockfd, SIOCGIFCONF, &ifc)) {
      perror ("ioctl SIOCFIFCONF");
      close (sockfd);
      free (ifc.ifc_req);
      return NULL;
    }
  } while (size <= ifc.ifc_len);


  /* Loop throught the interface list and get the IP address of each IF */
  for (ifr = ifc.ifc_req;
       (gchar *) ifr < (gchar *) ifc.ifc_req + ifc.ifc_len;
       ++ifr) {
    nice_debug ("Found interface : %s", ifr->ifr_name);
    interfaces = g_list_prepend (interfaces, g_strdup (ifr->ifr_name));
  }

  free (ifc.ifc_req);
  close (sockfd);

  return interfaces;
}

#ifdef HAVE_GETIFADDRS

GList *
nice_interfaces_get_local_interfaces (void)
{
  GList *interfaces = NULL;
  struct ifaddrs *ifa, *results;

  if (getifaddrs (&results) < 0) {
    nice_debug ("Failed to retrieve list of network interfaces with \"getifaddrs\": %s."
      "Trying to use fallback ...", strerror (errno));
    return get_local_interfaces_ioctl ();
  }

  /* Loop and get each interface the system has, one by one... */
  for (ifa = results; ifa; ifa = ifa->ifa_next) {
    /* no ip address from interface that is down */
    if ((ifa->ifa_flags & IFF_UP) == 0)
      continue;

    if (ifa->ifa_addr == NULL)
      continue;

    if (ifa->ifa_addr->sa_family == AF_INET || ifa->ifa_addr->sa_family == AF_INET6) {
      nice_debug ("Found interface : %s", ifa->ifa_name);
      interfaces = g_list_prepend (interfaces, g_strdup (ifa->ifa_name));
    }
  }

  freeifaddrs (results);

  return interfaces;
}

#else /* ! HAVE_GETIFADDRS */

GList *
nice_interfaces_get_local_interfaces (void)
{
  return get_local_interfaces_ioctl ();
}

#endif /* HAVE_GETIFADDRS */


static gboolean
nice_interfaces_is_private_ip (const struct sockaddr *sa)
{
  NiceAddress niceaddr;

  nice_address_init (&niceaddr);
  nice_address_set_from_sockaddr (&niceaddr, sa);
  return nice_address_is_private (&niceaddr);
}

static GList *
add_ip_to_list (GList *list, gchar *ip, gboolean append)
{
  GList *i;

  for (i = list; i; i = i->next) {
    gchar *addr = (gchar *) i->data;

    if (g_strcmp0 (addr, ip) == 0)
      return list;
  }
  if (append)
    return g_list_append (list, ip);
  else
    return g_list_prepend (list, ip);
}

static GList *
get_local_ips_ioctl (gboolean include_loopback)
{
  GList *ips = NULL;
  gint sockfd;
  gint size = 0;
  struct ifreq *ifr;
  struct ifconf ifc;
  union {
    struct sockaddr_in *sin;
    struct sockaddr *sa;
  } sa;
  
  GList *loopbacks = NULL;
#ifdef IGNORED_IFACE_PREFIX
  const gchar **prefix;
  gboolean ignored;
#endif

  if ((sockfd = socket (AF_INET, SOCK_DGRAM, IPPROTO_IP)) < 0) {
    nice_debug ("Error : Cannot open socket to retrieve interface list");
    return NULL;
  }

  ifc.ifc_len = 0;
  ifc.ifc_req = NULL;

  /* Loop and get each interface the system has, one by one... */
  do {
    size += sizeof (struct ifreq);
    /* realloc buffer size until no overflow occurs  */
    if (NULL == (ifc.ifc_req = realloc (ifc.ifc_req, size))) {
      nice_debug ("Error : Out of memory while allocation interface"
          " configuration structure");
      close (sockfd);
      return NULL;
    }
    ifc.ifc_len = size;

    if (ioctl (sockfd, SIOCGIFCONF, &ifc)) {
      perror ("ioctl SIOCFIFCONF");
      close (sockfd);
      free (ifc.ifc_req);
      return NULL;
    }
  } while  (size <= ifc.ifc_len);


  /* Loop throught the interface list and get the IP address of each IF */
  for (ifr = ifc.ifc_req;
       (gchar *) ifr < (gchar *) ifc.ifc_req + ifc.ifc_len;
       ++ifr) {

    if (ioctl (sockfd, SIOCGIFFLAGS, ifr)) {
      nice_debug ("Error : Unable to get IP information for interface %s."
          " Skipping...", ifr->ifr_name);
      continue;  /* failed to get flags, skip it */
    }

    /* no ip address from interface that is down */
    if ((ifr->ifr_flags & IFF_UP) == 0)
      continue;

    /* no ip address from interface that isn't running */
    if ((ifr->ifr_flags & IFF_RUNNING) == 0)
      continue;

    sa.sa = &ifr->ifr_addr;
    nice_debug ("Interface:  %s", ifr->ifr_name);
    nice_debug ("IP Address: %s", inet_ntoa (sa.sin->sin_addr));
    if ((ifr->ifr_flags & IFF_LOOPBACK) == IFF_LOOPBACK){
      if (include_loopback)
        loopbacks = add_ip_to_list (loopbacks, g_strdup (inet_ntoa (sa.sin->sin_addr)), TRUE);
      else
        nice_debug ("Ignoring loopback interface");
      continue;
    }

#ifdef IGNORED_IFACE_PREFIX
    ignored = FALSE;
    for (prefix = ignored_iface_prefix_list; *prefix; prefix++) {
      if (g_str_has_prefix (ifr->ifr_name, *prefix)) {
        nice_debug ("Ignoring interface %s as it matches prefix %s",
            ifr->ifr_name, *prefix);
        ignored = TRUE;
        break;
      }
    }

    if (ignored)
      continue;
#endif

    if (nice_interfaces_is_private_ip (sa.sa)) {
      ips = add_ip_to_list (ips, g_strdup (inet_ntoa (sa.sin->sin_addr)), TRUE);
    } else {
      ips = add_ip_to_list (ips, g_strdup (inet_ntoa (sa.sin->sin_addr)), FALSE);
    }
  }

  close (sockfd);
  free (ifc.ifc_req);

  if (loopbacks)
    ips = g_list_concat (ips, loopbacks);

  return ips;
}

#ifdef HAVE_GETIFADDRS

GList *
nice_interfaces_get_local_ips (gboolean include_loopback)
{
  GList *ips = NULL;
  struct ifaddrs *ifa, *results;
  GList *loopbacks = NULL;
#ifdef IGNORED_IFACE_PREFIX
  const gchar **prefix;
  gboolean ignored;
#endif

  if (getifaddrs (&results) < 0) {
    nice_debug ("Failed to retrieve list of network interfaces with \"getifaddrs\": %s."
      "Trying to use fallback ...", strerror (errno));
    return get_local_ips_ioctl (include_loopback);
  }

  /* Loop through the interface list and get the IP address of each IF */
  for (ifa = results; ifa; ifa = ifa->ifa_next) {
    gchar *addr_string;

    /* no ip address from interface that is down */
    if ((ifa->ifa_flags & IFF_UP) == 0)
      continue;

    /* no ip address from interface that isn't running */
    if ((ifa->ifa_flags & IFF_RUNNING) == 0)
      continue;

    if (ifa->ifa_addr == NULL)
      continue;

    /* Convert to a string. */
    addr_string = sockaddr_to_string (ifa->ifa_addr);
    if (addr_string == NULL) {
      nice_debug ("Failed to convert address to string for interface ‘%s’.",
          ifa->ifa_name);
      continue;
    }

    nice_debug ("Interface:  %s", ifa->ifa_name);
    nice_debug ("IP Address: %s", addr_string);
    if ((ifa->ifa_flags & IFF_LOOPBACK) == IFF_LOOPBACK) {
      if (include_loopback) {
        loopbacks = add_ip_to_list (loopbacks, addr_string, TRUE);
      } else {
        nice_debug ("Ignoring loopback interface");
        g_free (addr_string);
      }
      continue;
    }

#ifdef IGNORED_IFACE_PREFIX
    ignored = FALSE;
    for (prefix = ignored_iface_prefix_list; *prefix; prefix++) {
      if (g_str_has_prefix (ifa->ifa_name, *prefix)) {
        nice_debug ("Ignoring interface %s as it matches prefix %s",
            ifa->ifa_name, *prefix);
        g_free (addr_string);
        ignored = TRUE;
        break;
      }
    }

    if (ignored)
      continue;
#endif

    if (nice_interfaces_is_private_ip (ifa->ifa_addr))
      ips = add_ip_to_list (ips, addr_string, TRUE);
    else
      ips = add_ip_to_list (ips, addr_string, FALSE);
  }

  freeifaddrs (results);

  if (loopbacks)
    ips = g_list_concat (ips, loopbacks);

  return ips;
}

#else /* ! HAVE_GETIFADDRS */

GList *
nice_interfaces_get_local_ips (gboolean include_loopback)
{
  return get_local_ips_ioctl (include_loopback);
}

#endif /* HAVE_GETIFADDRS */

gchar *
nice_interfaces_get_ip_for_interface (gchar *interface_name)
{
  struct ifreq ifr;
  union {
    struct sockaddr *addr;
    struct sockaddr_in *in;
  } sa;
  gint sockfd;

  g_return_val_if_fail (interface_name != NULL, NULL);

  ifr.ifr_addr.sa_family = AF_INET;
  memset (ifr.ifr_name, 0, sizeof (ifr.ifr_name));
  g_strlcpy (ifr.ifr_name, interface_name, sizeof (ifr.ifr_name));

  if ((sockfd = socket (AF_INET, SOCK_DGRAM, IPPROTO_IP)) < 0) {
    nice_debug ("Error : Cannot open socket to retrieve interface list");
    return NULL;
  }

  if (ioctl (sockfd, SIOCGIFADDR, &ifr) < 0) {
    nice_debug ("Error : Unable to get IP information for interface %s",
      interface_name);
    close (sockfd);
    return NULL;
  }

  close (sockfd);
  sa.addr = &ifr.ifr_addr;
  nice_debug ("Address for %s: %s", interface_name, inet_ntoa (sa.in->sin_addr));
  return g_strdup (inet_ntoa (sa.in->sin_addr));
}

#else /* G_OS_UNIX */
#ifdef G_OS_WIN32

#include <winsock2.h>
#include <iphlpapi.h>

// Should be in Iphlpapi.h, but mingw doesn't seem to have these
// Values copied directly from:
// http://msdn.microsoft.com/en-us/library/aa366845(v=vs.85).aspx
// (Title: MIB_IPADDRROW structure)

#ifndef MIB_IPADDR_DISCONNECTED
#define MIB_IPADDR_DISCONNECTED 0x0008
#endif

#ifndef MIB_IPADDR_DELETED
#define MIB_IPADDR_DELETED 0x0040
#endif

static IP_ADAPTER_ADDRESSES *
_nice_get_adapters_addresses (void)
{
  IP_ADAPTER_ADDRESSES *addresses = NULL;
  ULONG status;
  guint iterations;
  ULONG addresses_size;

  /* As suggested on
   * http://msdn.microsoft.com/en-gb/library/windows/desktop/aa365915%28v=vs.85%29.aspx */
  #define MAX_TRIES 3
  #define INITIAL_BUFFER_SIZE 15000

  addresses_size = INITIAL_BUFFER_SIZE;
  iterations = 0;

  do {
    g_free (addresses);
    addresses = g_malloc0 (addresses_size);

    status = GetAdaptersAddresses (AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER, NULL, addresses, &addresses_size);
  } while ((status == ERROR_BUFFER_OVERFLOW) && (iterations++ < MAX_TRIES));

  nice_debug ("Queried addresses with status %lu.", status);

  #undef INITIAL_BUFFER_SIZE
  #undef MAX_TRIES

  /* Error? */
  if (status != NO_ERROR) {
    gchar *msg = g_win32_error_message (status);
    nice_debug ("Error retrieving local addresses: %s", msg);
    g_free (msg);
    g_free (addresses);
    return NULL;
  }

  return addresses;
}

GList *
nice_interfaces_get_local_interfaces (void)
{
  IP_ADAPTER_ADDRESSES *addresses, *a;
  GList *ret = NULL;

  addresses = _nice_get_adapters_addresses ();
  if (!addresses)
    return NULL;

  for (a = addresses; a != NULL; a = a->Next) {
    gchar *name = g_utf16_to_utf8 (a->FriendlyName, -1, NULL, NULL, NULL);
    ret = g_list_append (ret, name);
  }

  g_free(addresses);

  return ret;
}

GList *
nice_interfaces_get_local_ips (gboolean include_loopback)
{
  IP_ADAPTER_ADDRESSES *addresses, *a;
  DWORD pref = 0;
  GList *ret = NULL;
#ifdef IGNORED_IFACE_PREFIX
  const gchar **prefix;
  gboolean ignored;
  const char output[256];
#endif

  addresses = _nice_get_adapters_addresses ();
  if (!addresses)
    return NULL;

  /*
   * Get the best interface for transport to 0.0.0.0.
   * This interface should be first in list!
   */
  {
    DWORD retcode;
    struct sockaddr_in sa_any = {0};

    sa_any.sin_family = AF_INET;
    sa_any.sin_addr.s_addr = htonl (INADDR_ANY);

    retcode = GetBestInterfaceEx ((SOCKADDR *) &sa_any, &pref);
    if (retcode != NO_ERROR) {
      gchar *msg = g_win32_error_message (retcode);
      nice_debug ("Error fetching best interface: %s", msg);
      g_free (msg);
      pref = 0;
    }
  }

  /* Loop over the adapters. */
  for (a = addresses; a != NULL; a = a->Next) {
    IP_ADAPTER_UNICAST_ADDRESS *unicast;

    nice_debug ("Interface ‘%S’:", a->FriendlyName);

    /* Various conditions for ignoring the interface. */
    if (a->Flags & IP_ADAPTER_RECEIVE_ONLY ||
        a->OperStatus == IfOperStatusDown ||
        a->OperStatus == IfOperStatusNotPresent ||
        a->OperStatus == IfOperStatusLowerLayerDown) {
      nice_debug ("Rejecting interface due to being down or read-only.");
      continue;
    }

    if (!include_loopback &&
        a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      nice_debug ("Rejecting loopback interface ‘%S’.", a->FriendlyName);
      continue;
    }

#ifdef IGNORED_IFACE_PREFIX
    sprintf_s(output, 256, "%ws", a->FriendlyName);
    ignored = FALSE;
    for (prefix = ignored_iface_prefix_list; *prefix; prefix++) {
      if (g_str_has_prefix (output, *prefix)) {
        nice_debug ("Ignoring interface %s as it matches prefix %s",
            output, *prefix);
        ignored = TRUE;
        break;
      }
    }

    if (ignored)
      continue;
#endif

    /* Grab the interface’s unicast addresses. */
    for (unicast = a->FirstUnicastAddress;
         unicast != NULL; unicast = unicast->Next) {
      gchar *addr_string;

      addr_string = sockaddr_to_string (unicast->Address.lpSockaddr);
      if (addr_string == NULL) {
        nice_debug ("Failed to convert address to string for interface ‘%S’.",
            a->FriendlyName);
        continue;
      }

      nice_debug ("Adapter %S IP address: %s", a->FriendlyName, addr_string);

      if (a->IfIndex == pref || a->Ipv6IfIndex == pref)
        ret = g_list_prepend (ret, addr_string);
      else
        ret = g_list_append (ret, addr_string);
    }
  }

  g_free (addresses);

  return ret;
}

gchar *
nice_interfaces_get_ip_for_interface (gchar *interface_name)
{
  IP_ADAPTER_ADDRESSES *addresses, *a;
  IP_ADAPTER_UNICAST_ADDRESS *unicast;
  DWORD status;
  gchar * ret = NULL;

  addresses = _nice_get_adapters_addresses ();
  if (!addresses)
    return NULL;

  for (a = addresses; a != NULL; a = a->Next) {
    IP_ADAPTER_UNICAST_ADDRESS *unicast;
    gchar *name;

    /* Various conditions for ignoring the interface. */
    if (a->OperStatus == IfOperStatusDown ||
        a->OperStatus == IfOperStatusNotPresent ||
        a->OperStatus == IfOperStatusLowerLayerDown) {
      nice_debug ("Rejecting interface '%S' because it is down or not present",
          a->FriendlyName);
      continue;
    }

    name = g_utf16_to_utf8 (a->FriendlyName, -1, NULL, NULL, NULL);
    status = g_strcmp0 (interface_name, name);
    g_free (name);

    /* Found the adapter */
    if (status == 0)
      break;

    nice_debug ("Rejecting interface '%s' != '%s'", name, interface_name);
  }

  if (!a) {
    nice_debug ("No matches found for interface %s", interface_name);
    goto out;
  }

  /* Grab the interface’s ipv4 unicast addresses. */
  for (unicast = a->FirstUnicastAddress;
       unicast != NULL; unicast = unicast->Next) {
    if (unicast->Address.lpSockaddr->sa_family != AF_INET) {
      nice_debug ("Rejecting ipv6 address on interface %S", a->FriendlyName);
      continue;
    }

    ret = sockaddr_to_string (unicast->Address.lpSockaddr);
    if (ret == NULL) {
      nice_debug ("Failed to convert address to string for interface: %S",
          a->FriendlyName);
      continue;
    }

    nice_debug ("Adapter %S IP address: %s", a->FriendlyName, ret);
    break;
  }

out:
  g_free (addresses);

  return ret;
}


#else /* G_OS_WIN32 */
#error Can not use this method for retreiving ip list from OS other than unix or windows
#endif /* G_OS_WIN32 */
#endif /* G_OS_UNIX */
