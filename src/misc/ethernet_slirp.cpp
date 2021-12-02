/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2021  The DOSBox Staging Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "config.h"

#if C_SLIRP

#include <algorithm>
#include <time.h>

#include "dosbox.h"
#include "ethernet_slirp.h"
#include "setup.h"
#include "support.h"

#ifdef WIN32
#include <ws2tcpip.h>
#else /* !WIN32 */
#include <arpa/inet.h>
#endif /* WIN32 */

/* Begin boilerplate to map libslirp's C-based callbacks to our C++
 * object. The user data is provided inside the 'opaque' pointer.
 */

ssize_t slirp_receive_packet(const void *buf, size_t len, void *opaque)
{
	auto conn = static_cast<SlirpEthernetConnection *>(opaque);
	const auto packets = check_cast<uint16_t>(len);
	conn->ReceivePacket(static_cast<const uint8_t *>(buf), packets);
	return packets;
}

void slirp_guest_error(const char *msg, [[maybe_unused]] void *opaque)
{
	LOG_MSG("SLIRP: Slirp error: %s", msg);
}

int64_t slirp_clock_get_ns([[maybe_unused]] void *opaque)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	/* if clock_gettime fails we have more serious problems */
	return ts.tv_nsec + (ts.tv_sec * 1'000'000'000LL);
}

void *slirp_timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque)
{
	auto conn = static_cast<SlirpEthernetConnection *>(opaque);
	return conn->TimerNew(cb, cb_opaque);
}

void slirp_timer_free(void *timer, void *opaque)
{
	auto conn = static_cast<SlirpEthernetConnection *>(opaque);
	struct slirp_timer *real_timer = (struct slirp_timer *)timer;
	conn->TimerFree(real_timer);
}

void slirp_timer_mod(void *timer, int64_t expire_time, void *opaque)
{
	auto conn = static_cast<SlirpEthernetConnection *>(opaque);
	struct slirp_timer *real_timer = (struct slirp_timer *)timer;
	conn->TimerMod(real_timer, expire_time);
}

int slirp_add_poll(int fd, int events, void *opaque)
{
	auto conn = static_cast<SlirpEthernetConnection *>(opaque);
	return conn->PollAdd(fd, events);
}

int slirp_get_revents(int idx, void *opaque)
{
	auto conn = static_cast<SlirpEthernetConnection *>(opaque);
	return conn->PollGetSlirpRevents(idx);
}

void slirp_register_poll_fd(int fd, void *opaque)
{
	auto conn = static_cast<SlirpEthernetConnection *>(opaque);
	conn->PollRegister(fd);
}

void slirp_unregister_poll_fd(int fd, void *opaque)
{
	auto conn = static_cast<SlirpEthernetConnection *>(opaque);
	conn->PollUnregister(fd);
}

void slirp_notify([[maybe_unused]] void *opaque)
{
	// empty, function is provided for API compliance
}

/* End boilerplate */

SlirpEthernetConnection::SlirpEthernetConnection()
        : EthernetConnection(),
          config(),
          timers(),
          get_packet_callback(),
          registered_fds(),
#ifdef WIN32
          readfds(),
          writefds(),
          exceptfds()
#else /* !WIN32 */
          polls()
#endif
{
	slirp_callbacks.send_packet = slirp_receive_packet;
	slirp_callbacks.guest_error = slirp_guest_error;
	slirp_callbacks.clock_get_ns = slirp_clock_get_ns;
	slirp_callbacks.timer_new = slirp_timer_new;
	slirp_callbacks.timer_free = slirp_timer_free;
	slirp_callbacks.timer_mod = slirp_timer_mod;
	slirp_callbacks.register_poll_fd = slirp_register_poll_fd;
	slirp_callbacks.unregister_poll_fd = slirp_unregister_poll_fd;
	slirp_callbacks.notify = slirp_notify;
}

SlirpEthernetConnection::~SlirpEthernetConnection()
{
	if (slirp)
		slirp_cleanup(slirp);
}

bool SlirpEthernetConnection::Initialize([[maybe_unused]] Section *dosbox_config)
{
	LOG_MSG("SLIRP: Slirp version: %s", slirp_version_string());

	/* Config */
	config.version = 1;

	// If true, prevents the guest from accessing the host, which will cause
	// libslrip's internal DHCP server to fail.
	config.restricted = false;

	// If true, prevent the guest access from accessing the host's loopback
	// interfaces.
	config.disable_host_loopback = false;

	// The maximum transmission unit for Ethernet packets transmitted from
	// the guest. 0 is default.
	config.if_mtu = 0;

	// The maximum recieve unit for Ethernet packets transmitted to the
	// guest. 0 is default.
	config.if_mru = 0;

	config.enable_emu = 0; // buggy - keep this at 0
	config.in_enabled = 1;

	// The IPv4 network the guest and host services are on
	inet_pton(AF_INET, "10.0.2.0", &config.vnetwork);

	// The netmask for the IPv4 network.
	inet_pton(AF_INET, "255.255.255.0", &config.vnetmask);
	inet_pton(AF_INET, "10.0.2.2", &config.vhost);
	inet_pton(AF_INET, "10.0.2.3", &config.vnameserver);
	inet_pton(AF_INET, "10.0.2.15", &config.vdhcp_start);

	/* IPv6 code is left here as reference but disabled as no DOS-era
	 * software supports it and might get confused by it */
	config.in6_enabled = 0;
	inet_pton(AF_INET6, "fec0::", &config.vprefix_addr6);
	config.vprefix_len = 64;
	inet_pton(AF_INET6, "fec0::2", &config.vhost6);
	inet_pton(AF_INET6, "fec0::3", &config.vnameserver6);

	/* DHCPv4, BOOTP, TFTP */
	config.vhostname = "dosbox-staging";
	config.vdnssearch = NULL;
	config.vdomainname = NULL;
	config.tftp_server_name = NULL;
	config.tftp_path = NULL;
	config.bootfile = NULL;

	slirp = slirp_new(&config, &slirp_callbacks, this);
	if (slirp) {
		LOG_MSG("SLIRP: Successfully initialized");
		return true;
	} else {
		LOG_MSG("SLIRP: Failed to initialize");
		return false;
	}
}

void SlirpEthernetConnection::SendPacket(const uint8_t *packet, int len)
{
	slirp_input(slirp, packet, len);
}

void SlirpEthernetConnection::GetPackets(std::function<void(const uint8_t *, int)> callback)
{
	get_packet_callback = callback;
	uint32_t timeout_ms = 0;
	PollsClear();
	PollsAddRegistered();
	slirp_pollfds_fill(slirp, &timeout_ms, slirp_add_poll, this);
	bool poll_failed = !PollsPoll(timeout_ms);
	slirp_pollfds_poll(slirp, poll_failed, slirp_get_revents, this);
	TimersRun();
}

void SlirpEthernetConnection::ReceivePacket(const uint8_t *packet, int len)
{
	get_packet_callback(packet, len);
}

struct slirp_timer *SlirpEthernetConnection::TimerNew(SlirpTimerCb cb, void *cb_opaque)
{
	struct slirp_timer *timer = new struct slirp_timer;
	timer->expires_ns = 0;
	timer->cb = cb;
	timer->cb_opaque = cb_opaque;
	timers.push_back(timer);
	return timer;
}

void SlirpEthernetConnection::TimerFree(struct slirp_timer *timer)
{
	std::remove(timers.begin(), timers.end(), timer);
	delete timer;
}

void SlirpEthernetConnection::TimerMod(struct slirp_timer *timer, int64_t expire_time_ms)
{
	/* expire_time is in milliseconds despite slirp wanting a nanosecond
	 * clock */
	timer->expires_ns = expire_time_ms * 1'000'000;
}

void SlirpEthernetConnection::TimersRun()
{
	int64_t now = slirp_clock_get_ns(NULL);
	for (struct slirp_timer *timer : timers) {
		if (timer->expires_ns && timer->expires_ns < now) {
			timer->expires_ns = 0;
			timer->cb(timer->cb_opaque);
		}
	}
}

void SlirpEthernetConnection::TimersClear()
{
	for (auto *timer : timers)
		delete timer;
	timers.clear();
}

void SlirpEthernetConnection::PollRegister(int fd)
{
#ifdef WIN32
	/* BUG: Skip this entirely on Win32 as libslirp gives us invalid fds. */
	return;
#endif
	PollUnregister(fd);
	registered_fds.push_back(fd);
}

void SlirpEthernetConnection::PollUnregister(int fd)
{
	std::remove(registered_fds.begin(), registered_fds.end(), fd);
}

void SlirpEthernetConnection::PollsAddRegistered()
{
	for (int fd : registered_fds) {
		PollAdd(fd, SLIRP_POLL_IN | SLIRP_POLL_OUT);
	}
}

/* Begin the bulk of the platform-specific code.
 * This mostly involves handling data structures and mapping
 * libslirp's view of our polling system to whatever we use
 * internally.
 * libslirp really wants poll() as it gives information about
 * out of band TCP data and connection hang-ups.
 * This is easy to do on Unix, but on other systems it needs
 * custom implementations that give this data. */

#ifndef WIN32

void SlirpEthernetConnection::PollsClear()
{
	polls.clear();
}

int SlirpEthernetConnection::PollAdd(int fd, int slirp_events)
{
	int16_t real_events = 0;
	if (slirp_events & SLIRP_POLL_IN)
		real_events |= POLLIN;
	if (slirp_events & SLIRP_POLL_OUT)
		real_events |= POLLOUT;
	if (slirp_events & SLIRP_POLL_PRI)
		real_events |= POLLPRI;
	struct pollfd new_poll;
	new_poll.fd = fd;
	new_poll.events = real_events;
	polls.push_back(new_poll);
	return (check_cast<int>(polls.size() - 1));
}

bool SlirpEthernetConnection::PollsPoll(uint32_t timeout_ms)
{
	const auto ret = poll(polls.data(), polls.size(), static_cast<int>(timeout_ms));
	return (ret > -1);
}

int SlirpEthernetConnection::PollGetSlirpRevents(int idx)
{
	assert(idx >= 0 && idx < static_cast<int>(polls.size()));
	const auto real_revents = polls.at(static_cast<size_t>(idx)).revents;
	int slirp_revents = 0;
	if (real_revents & POLLIN)
		slirp_revents |= SLIRP_POLL_IN;
	if (real_revents & POLLOUT)
		slirp_revents |= SLIRP_POLL_OUT;
	if (real_revents & POLLPRI)
		slirp_revents |= SLIRP_POLL_PRI;
	if (real_revents & POLLERR)
		slirp_revents |= SLIRP_POLL_ERR;
	if (real_revents & POLLHUP)
		slirp_revents |= SLIRP_POLL_HUP;
	return slirp_revents;
}

#else

void SlirpEthernetConnection::PollsClear()
{
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_ZERO(&exceptfds);
}

int SlirpEthernetConnection::PollAdd(int fd, int slirp_events)
{
	if (slirp_events & SLIRP_POLL_IN)
		FD_SET(fd, &readfds);
	if (slirp_events & SLIRP_POLL_OUT)
		FD_SET(fd, &writefds);
	if (slirp_events & SLIRP_POLL_PRI)
		FD_SET(fd, &exceptfds);
	return fd;
}

bool SlirpEthernetConnection::PollsPoll(uint32_t timeout_ms)
{
	struct timeval timeout;
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	int ret = select(0, &readfds, &writefds, &exceptfds, &timeout);
	return (ret > -1);
}

int SlirpEthernetConnection::PollGetSlirpRevents(int idx)
{
	/* Windows does not support poll(). It has WSAPoll() but this is
	 * reported as broken by libcurl and other projects, and Microsoft
	 * doesn't seem to want to fix this any time soon.
	 * glib provides g_poll() but that doesn't seem to work either.
	 * The solution I've made uses plain old select(), but checks for
	 * extra conditions and adds those to the flags we pass to libslirp.
	 * There's no one-to-one mapping of poll() flags on Windows, so here's
	 * my definition:
	 * SLIRP_POLL_HUP: The remote closed the socket gracefully.
	 * SLIRP_POLL_ERR: An exception happened or reading failed
	 * SLIRP_POLL_PRI: TCP Out-of-band data available
	 */
	int slirp_revents = 0;
	if (FD_ISSET(idx, &readfds)) {
		/* This code is broken on ReactOS peeking a closed socket
		 * will cause the next recv() to fail instead of acting
		 * normally. See CORE-17425 on their JIRA */
		char buf[8];
		int read = recv(idx, buf, sizeof(buf), MSG_PEEK);
		int error = (read == SOCKET_ERROR) ? WSAGetLastError() : 0;
		if (read > 0 || error == WSAEMSGSIZE) {
			slirp_revents |= SLIRP_POLL_IN;
		} else if (read == 0) {
			slirp_revents |= SLIRP_POLL_IN;
			slirp_revents |= SLIRP_POLL_HUP;
		} else {
			slirp_revents |= SLIRP_POLL_IN;
			slirp_revents |= SLIRP_POLL_ERR;
		}
	}
	if (FD_ISSET(idx, &writefds)) {
		slirp_revents |= SLIRP_POLL_OUT;
	}
	if (FD_ISSET(idx, &exceptfds)) {
		u_long atmark = 0;
		if (ioctlsocket(idx, SIOCATMARK, &atmark) == 0 && atmark == 1) {
			slirp_revents |= SLIRP_POLL_PRI;
		} else {
			slirp_revents |= SLIRP_POLL_ERR;
		}
	}
	return slirp_revents;
}

#endif

#endif