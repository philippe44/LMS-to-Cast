/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *	(c) Philippe 2015-2017, philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "squeezelite.h"

#if LINUX || OSX || FREEBSD
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netdb.h>
#if FREEBSD
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#endif
#endif
#if WIN
#include <iphlpapi.h>
#endif
#if OSX
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <sys/time.h>
#endif

#include <fcntl.h>

extern log_level 	util_loglevel;
static log_level 	*loglevel = &util_loglevel;

// cmdline parsing
char *next_param(char *src, char c) {
	static char *str = NULL;
	char *ptr, *ret;
	if (src) str = src;
 	if (str && (ptr = strchr(str, c))) {
		ret = str;
		*ptr = '\0';
		str = ptr + 1;
	} else {
		ret = str;
		str = NULL;
	}

	return ret && ret[0] ? ret : NULL;
}

// clock
u32_t gettime_ms(void) {
#if WIN
	return GetTickCount();
#else
#if LINUX || FREEBSD
	struct timespec ts;
	if (!clock_gettime(CLOCK_MONOTONIC, &ts)) {
		return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	}
#endif
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

// mutex wait with timeout
#if LINUX || FREEBSD
int _mutex_timedlock(pthread_mutex_t *m, u32_t ms_wait)
{
	int rc = -1;
	struct timespec ts;

	if (!clock_gettime(CLOCK_REALTIME, &ts)) {
		ts.tv_nsec += (ms_wait % 1000) * 1000000;
		ts.tv_sec += ms_wait / 1000 + (ts.tv_nsec / 1000000000);
		ts.tv_nsec = ts.tv_nsec % 1000000000;
		rc = pthread_mutex_timedlock(m, &ts);
	}
	return rc;
}
#endif

#if WIN || OSX
int _mutex_timedlock(pthread_mutex_t *m, u32_t ms_wait)
{
	int rc;
	s32_t wait = (s32_t) ms_wait;

	/* Try to acquire the lock and, if we fail, sleep for 10ms. */
	while (((rc = pthread_mutex_trylock (m)) == EBUSY) && (wait > 0)) {
		wait -= 10;
		usleep(10000);
	}

	return rc;
}
#endif


#if WIN
/*----------------------------------------------------------------------------*/
int asprintf(char **strp, const char *fmt, ...)
{
	va_list args, cp;
	int len, ret = 0;

	va_start(args, fmt);
	len = vsnprintf(NULL, 0, fmt, args);
	*strp = malloc(len + 1);

	if (*strp) ret = vsprintf(*strp, fmt, args);

	va_end(args);

	return ret;
}
#endif


/*----------------------------------------------------------------------------*/
/* 																			  */
/* NETWORK management														  */
/* 																			  */
/*----------------------------------------------------------------------------*/

// mac address
#if LINUX
// search first 4 interfaces returned by IFCONF
void get_mac(u8_t mac[]) {
    struct ifconf ifc;
    struct ifreq *ifr, *ifend;
    struct ifreq ifreq;
    struct ifreq ifs[4];

	mac[0] = mac[1] = mac[2] = mac[3] = mac[4] = mac[5] = 0;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
 
    ifc.ifc_len = sizeof(ifs);
    ifc.ifc_req = ifs;

    if (ioctl(s, SIOCGIFCONF, &ifc) == 0) {
		ifend = ifs + (ifc.ifc_len / sizeof(struct ifreq));

		for (ifr = ifc.ifc_req; ifr < ifend; ifr++) {
			if (ifr->ifr_addr.sa_family == AF_INET) {

				strncpy(ifreq.ifr_name, ifr->ifr_name, sizeof(ifreq.ifr_name));
				if (ioctl (s, SIOCGIFHWADDR, &ifreq) == 0) {
					memcpy(mac, ifreq.ifr_hwaddr.sa_data, 6);
					if (mac[0]+mac[1]+mac[2] != 0) {
						break;
					}
				}
			}
		}
	}

	close(s);
}
#endif

#if OSX || FREEBSD
void get_mac(u8_t mac[]) {
	struct ifaddrs *addrs, *ptr;
	const struct sockaddr_dl *dlAddr;
	const unsigned char *base;
	
	mac[0] = mac[1] = mac[2] = mac[3] = mac[4] = mac[5] = 0;
	
	if (getifaddrs(&addrs) == 0) {
		ptr = addrs;
		while (ptr) {
			if (ptr->ifa_addr->sa_family == AF_LINK && ((const struct sockaddr_dl *) ptr->ifa_addr)->sdl_type == IFT_ETHER) {
				dlAddr = (const struct sockaddr_dl *)ptr->ifa_addr;
				base = (const unsigned char*) &dlAddr->sdl_data[dlAddr->sdl_nlen];
				memcpy(mac, base, min(dlAddr->sdl_alen, 6));
				break;
			}
			ptr = ptr->ifa_next;
		}
		freeifaddrs(addrs);
	}
}
#endif

#if WIN
#pragma comment(lib, "IPHLPAPI.lib")
void get_mac(u8_t mac[]) {
    IP_ADAPTER_INFO AdapterInfo[16];
    DWORD dwBufLen = sizeof(AdapterInfo);
    DWORD dwStatus = GetAdaptersInfo(AdapterInfo, &dwBufLen);
	
	mac[0] = mac[1] = mac[2] = mac[3] = mac[4] = mac[5] = 0;

	if (GetAdaptersInfo(AdapterInfo, &dwBufLen) == ERROR_SUCCESS) {
		memcpy(mac, AdapterInfo[0].Address, 6);
	}
}
#endif

void set_nonblock(sockfd s) {
#if WIN
	u_long iMode = 1;
	ioctlsocket(s, FIONBIO, &iMode);
#else
	int flags = fcntl(s, F_GETFL,0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

// connect for socket already set to non blocking with timeout in ms
int connect_timeout(sockfd sock, const struct sockaddr *addr, socklen_t addrlen, int timeout) {
	fd_set w, e;
	struct timeval tval;

	if (connect(sock, addr, addrlen) < 0) {
#if !WIN
		if (last_error() != EINPROGRESS) {
#else
		if (last_error() != WSAEWOULDBLOCK) {
#endif
			return -1;
		}
	}

	FD_ZERO(&w);
	FD_SET(sock, &w);
	e = w;
	tval.tv_sec = timeout / 1000;
	tval.tv_usec = (timeout - tval.tv_sec * 1000) * 1000;

	// only return 0 if w set and sock error is zero, otherwise return error code
	if (select(sock + 1, NULL, &w, &e, timeout ? &tval : NULL) == 1 && FD_ISSET(sock, &w)) {
		int	error = 0;
		socklen_t len = sizeof(error);
		getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&error, &len);
		return error;
	}

	return -1;
}

void server_addr(char *server, in_addr_t *ip_ptr, unsigned *port_ptr) {
	struct addrinfo *res = NULL;
	struct addrinfo hints;
	const char *port = NULL;
	
	if (strtok(server, ":")) {
		port = strtok(NULL, ":");
		if (port) {
			*port_ptr = atoi(port);
		}
	}

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	
	getaddrinfo(server, NULL, &hints, &res);
	
	if (res && res->ai_addr) {
		*ip_ptr = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
	} 
	
	if (res) {
		freeaddrinfo(res);
	}
}

void set_readwake_handles(event_handle handles[], sockfd s, event_event e) {
#if WINEVENT
	handles[0] = WSACreateEvent();
	handles[1] = e;
	WSAEventSelect(s, handles[0], FD_READ | FD_CLOSE);
#elif SELFPIPE
	handles[0].fd = s;
	handles[1].fd = e.fds[0];
	handles[0].events = POLLIN;
	handles[1].events = POLLIN;
#else
	handles[0].fd = s;
	handles[1].fd = e;
	handles[0].events = POLLIN;
	handles[1].events = POLLIN;
#endif
}

event_type wait_readwake(event_handle handles[], int timeout) {
#if WINEVENT
	int wait = WSAWaitForMultipleEvents(2, handles, false, timeout, false);
	if (wait == WSA_WAIT_EVENT_0) {
		WSAResetEvent(handles[0]);
		return EVENT_READ;
	} else if (wait == WSA_WAIT_EVENT_0 + 1) {
		return EVENT_WAKE;
	} else {
		return EVENT_TIMEOUT;
	}
#else
	if (poll(handles, 2, timeout) > 0) {
		if (handles[0].revents) {
			return EVENT_READ;
		}
		if (handles[1].revents) {
			wake_clear(handles[1].fd);
			return EVENT_WAKE;
		}
	}
	return EVENT_TIMEOUT;
#endif
}

// pack/unpack to network byte order
void packN(u32_t *dest, u32_t val) {
	u8_t *ptr = (u8_t *)dest;
	*(ptr)   = (val >> 24) & 0xFF; *(ptr+1) = (val >> 16) & 0xFF; *(ptr+2) = (val >> 8) & 0xFF;	*(ptr+3) = val & 0xFF;
}

void packn(u16_t *dest, u16_t val) {
	u8_t *ptr = (u8_t *)dest;
	*(ptr) = (val >> 8) & 0xFF; *(ptr+1) = val & 0xFF;
}

u32_t unpackN(u32_t *src) {
	u8_t *ptr = (u8_t *)src;
	return *(ptr) << 24 | *(ptr+1) << 16 | *(ptr+2) << 8 | *(ptr+3);
} 

u16_t unpackn(u16_t *src) {
	u8_t *ptr = (u8_t *)src;
	return *(ptr) << 8 | *(ptr+1);
} 

#if OSX
void set_nosigpipe(sockfd s) {
	int set = 1;
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));
}
#endif

#if WIN
void *dlopen(const char *filename, int flag) {
	SetLastError(0);
	return LoadLibrary((LPCTSTR)filename);
}

void *dlsym(void *handle, const char *symbol) {
	SetLastError(0);
	return (void *)GetProcAddress(handle, symbol);
}

char *dlerror(void) {
	static char ret[32];
	int last = GetLastError();
	if (last) {
		sprintf(ret, "code: %i", last);
		SetLastError(0);
		return ret;
	}
	return NULL;
}

// this only implements numfds == 1
int poll(struct pollfd *fds, unsigned long numfds, int timeout) {
	fd_set r, w;
	struct timeval tv;
	int ret;
	
	FD_ZERO(&r);
	FD_ZERO(&w);
	
	if (fds[0].events & POLLIN) FD_SET(fds[0].fd, &r);
	if (fds[0].events & POLLOUT) FD_SET(fds[0].fd, &w);
	
	tv.tv_sec = timeout / 1000;
	tv.tv_usec = 1000 * (timeout % 1000);
	
	ret = select(fds[0].fd + 1, &r, &w, NULL, &tv);
    
	if (ret < 0) return ret;
	
	fds[0].revents = 0;
	if (FD_ISSET(fds[0].fd, &r)) fds[0].revents |= POLLIN;
	if (FD_ISSET(fds[0].fd, &w)) fds[0].revents |= POLLOUT;
	
	return ret;
}

#endif

#if LINUX || FREEBSD
void touch_memory(u8_t *buf, size_t size) {
	u8_t *ptr;
	for (ptr = buf; ptr < buf + size; ptr += sysconf(_SC_PAGESIZE)) {
		*ptr = 0;
	}
}
#endif

#if LINUX
int SendARP(in_addr_t src, in_addr_t dst, u8_t mac[], unsigned long *size) {
	int                 s;
	struct arpreq       areq;
	struct sockaddr_in *sin;

	/* Get an internet domain socket. */
	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		return -1;
	}

	/* Make the ARP request. */
	memset(&areq, 0, sizeof(areq));
	sin = (struct sockaddr_in *) &areq.arp_pa;
	sin->sin_family = AF_INET;

	sin->sin_addr.s_addr = src;
	sin = (struct sockaddr_in *) &areq.arp_ha;
	sin->sin_family = ARPHRD_ETHER;

	strncpy(areq.arp_dev, "eth0", 15);

	if (ioctl(s, SIOCGARP, (caddr_t) &areq) == -1) {
		return -1;
	}

	memcpy(mac, &(areq.arp_ha.sa_data), *size);
	return 0;
}
#elif OSX
int SendARP(in_addr_t src, in_addr_t dst, u8_t mac[], unsigned long *size)
{
	int mib[6];
	size_t needed;
	char *lim, *buf, *next;
	struct rt_msghdr *rtm;
	struct sockaddr_inarp *sin;
	struct sockaddr_dl *sdl;
	int found_entry = -1;

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;
	mib[3] = AF_INET;
	mib[4] = NET_RT_FLAGS;
	mib[5] = RTF_LLINFO;

	if (sysctl(mib, 6, NULL, &needed, NULL, 0) < 0)
		return (found_entry);

	if ((buf = malloc(needed)) == NULL)
		return (found_entry);

	if (sysctl(mib, 6, buf, &needed, NULL, 0) < 0)
		return (found_entry);

	lim = buf + needed;
	for (next = buf; next < lim; next += rtm->rtm_msglen)
	{
		rtm = (struct rt_msghdr *)next;
		sin = (struct sockaddr_inarp *)(rtm + 1);
		sdl = (struct sockaddr_dl *)(sin + 1);

		if (src)
		{
			if (src != sin->sin_addr.s_addr)
				continue;
		}

		if (sdl->sdl_alen)
		{
			found_entry = 0;
			memcpy(mac,  LLADDR(sdl), sdl->sdl_alen);
		}
	}

	free(buf);
	return (found_entry);
}
#elif !WIN
int SendARP(in_addr_t src, in_addr_t dst, u8_t mac[], unsigned long *size)
{
	LOG_ERROR("No SendARP build for this platform", NULL);
	return 1;
}
#endif

#if LINUX || FREEBSD || OSX
char *GetTempPath(u16_t size, char *path)
{
	strncpy(path, P_tmpdir, size);
	if (!strlen(path)) strncpy(path, "/var/tmp", size);
	path[size - 1] = '\0';
	return path;
}
#endif

/*----------------------------------------------------------------------------*/
int bind_socket(unsigned short *port, int mode)
{
	int sock;
	socklen_t len = sizeof(struct sockaddr);
	struct sockaddr_in addr;

	if ((sock = socket(AF_INET, mode, 0)) < 0) {
		LOG_ERROR("cannot create socket %d", sock);
		return sock;
	}

	/*  Populate socket address structure  */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port        = htons(*port);
#ifdef SIN_LEN
	si.sin_len = sizeof(si);
#endif

	if (bind(sock, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
		closesocket(sock);
		LOG_ERROR("cannot bind socket %d", sock);
		return -1;
	}

	if (!*port) {
		getsockname(sock, (struct sockaddr *) &addr, &len);
		*port = ntohs(addr.sin_port);
	}

	LOG_DEBUG("socket binding %d on port %d", sock, *port);

	return sock;
}

/*----------------------------------------------------------------------------*/
int shutdown_socket(int sd)
{
	if (sd <= 0) return -1;

#ifdef WIN32
	shutdown(sd, SD_BOTH);
#else
	shutdown(sd, SHUT_RDWR);
#endif

	LOG_DEBUG("closed socket %d", sd);

	return closesocket(sd);
}


/*----------------------------------------------------------------------------*/
/* 																			  */
/* HTTP management														 	  */
/* 																			  */
/*----------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
char *ltrim(char *s)
{
	while(isspace(*s)) s++;
	return s;
}

/*---------------------------------------------------------------------------*/
char *rtrim(char *s)
{
	char* back = s + strlen(s);
	while(isspace(*--back));
	*(back+1) = '\0';
	return s;
}

/*----------------------------------------------------------------------------*/
bool http_parse(int sock, char *request, char *method, key_data_t *rkd, char **body, int *len)
{
	char line[256], *dp;
	unsigned j;
	int i, timeout = 100;

	if ((i = read_line(sock, line, sizeof(line), timeout)) <= 0) {
		if (i < 0) {
			LOG_ERROR("cannot read method", NULL);
		}
		return false;
	}

	if (request) strcpy(request, line);

	if (!sscanf(line, "%s", method)) {
		LOG_ERROR("missing method", NULL);
		return false;
	}

	i = *len = 0;
	rkd[0].key = NULL;

	while (read_line(sock, line, sizeof(line), timeout) > 0) {

		// line folding should be deprecated
		if (i && rkd[i].key && (line[0] == ' ' || line[0] == '\t')) {
			for(j = 0; j < strlen(line); j++) if (line[j] != ' ' && line[j] != '\t') break;
			rkd[i].data = realloc(rkd[i].data, strlen(rkd[i].data) + strlen(line + j) + 1);
			strcat(rkd[i].data, line + j);
			continue;
		}

		dp = strstr(line,":");

		if (!dp){
			LOG_ERROR("Request failed, bad header", NULL);
			kd_free(rkd);
			return false;
		}

		*dp = 0;
		rkd[i].key = strdup(line);
		rkd[i].data = strdup(ltrim(dp + 1));

		if (!strcasecmp(rkd[i].key, "Content-Length")) *len = atol(rkd[i].data);

		i++;
		rkd[i].key = NULL;
	}

	if (*len) {
		int size = 0;

		*body = malloc(*len + 1);
		while (*body && size < *len) {
			int bytes = recv(sock, *body + size, *len - size, 0);
			if (bytes <= 0) break;
			size += bytes;
		}

		(*body)[*len] = '\0';

		if (!*body || size != *len) {
			LOG_ERROR("content length receive error %d %d", *len, size);
		}
	}

	return true;
}


/*----------------------------------------------------------------------------*/
int read_line(int fd, char *line, int maxlen, int timeout)
{
	int i,rval;
	int count=0;
	struct pollfd pfds;
	char ch;

	*line = 0;
	pfds.fd = fd;
	pfds.events = POLLIN;

	for(i = 0; i < maxlen; i++){
		if (poll(&pfds, 1, timeout)) rval=recv(fd, &ch, 1, 0);
		else return 0;

		if (rval == -1) {
			if (errno == EAGAIN) return 0;
			LOG_ERROR("fd: %d read error: %s", fd, strerror(errno));
			return -1;
		}

		if (rval == 0) {
			LOG_INFO("disconnected on the other end %u", fd);
			return 0;
		}

		if (ch == '\n') {
			*line=0;
			return count;
		}

		if (ch=='\r') continue;

		*line++=ch;
		count++;
		if (count >= maxlen-1) break;
	}

	*line = 0;
	return count;
}


/*----------------------------------------------------------------------------*/
char *http_send(int sock, char *method, key_data_t *rkd)
{
	unsigned sent, len;
	char *resp = kd_dump(rkd);
	char *data = malloc(strlen(method) + 2 + strlen(resp) + 2 + 1);

	len = sprintf(data, "%s\r\n%s\r\n", method, resp);
	NFREE(resp);

	sent = send(sock, data, len, 0);

	if (sent != len) {
		LOG_ERROR("HTTP send() error:%s %u (strlen=%u)", data, sent, len);
		NFREE(data);
	}

	return data;
}


/*----------------------------------------------------------------------------*/
char *kd_lookup(key_data_t *kd, char *key)
{
	int i = 0;
	while (kd && kd[i].key){
		if (!strcasecmp(kd[i].key, key)) return kd[i].data;
		i++;
	}
	return NULL;
}


/*----------------------------------------------------------------------------*/
bool kd_add(key_data_t *kd, char *key, char *data)
{
	int i = 0;
	while (kd && kd[i].key) i++;

	kd[i].key = strdup(key);
	kd[i].data = strdup(data);
	kd[i+1].key = NULL;

	return NULL;
}


/*----------------------------------------------------------------------------*/
void kd_free(key_data_t *kd)
{
	int i = 0;
	while (kd && kd[i].key){
		free(kd[i].key);
		if (kd[i].data) free(kd[i].data);
		i++;
	}

	kd[0].key = NULL;
}


/*----------------------------------------------------------------------------*/
char *kd_dump(key_data_t *kd)
{
	int i = 0;
	int pos = 0, size = 0;
	char *str = NULL;

	if (!kd) return strdup("\r\n");

	while (kd && kd[i].key) {
		char *buf;
		int len;

		len = asprintf(&buf, "%s: %s\r\n", kd[i].key, kd[i].data);

		while (pos + len >= size) {
			void *p = realloc(str, size + 1024);
			size += 1024;
			if (!p) {
				free(str);
				return NULL;
			}
			str = p;
		}

		memcpy(str + pos, buf, len);

		pos += len;
		free(buf);
		i++;
	}

	str[pos] = '\0';

	return str;
}




