/*
  This code is a modification of the original Eventlog to Syslog Script written by
  Curtis Smith of Purdue University. The original copyright notice can be found below.
  
  The original program was modified by Sherwin Faria for Rochester Institute of Technology
  in July 2009 to provide bug fixes and add several new features. Additions include
  the ability to ignore specific events, add the event timestamp to outgoing messages,
  a service status file, and compatibility with the new Vista/2k8 Windows Events service.

     Sherwin Faria
	 Rochester Institute of Technology
	 Information & Technology Services Bldg. 10
	 1 Lomb Memorial Drive
	 Rochester, NY 14623 U.S.A.
	 
	Send all comments, suggestions, or bug reports to:
		sherwin.faria@gmail.com
*/
 
/*
  Copyright (c) 1998-2007, Purdue University
  All rights reserved.

  Redistribution and use in source and binary forms are permitted provided
  that:

  (1) source distributions retain this entire copyright notice and comment,
      and
  (2) distributions including binaries display the following acknowledgement:

         "This product includes software developed by Purdue University."

      in the documentation or other materials provided with the distribution
      and in all advertising materials mentioning features or use of this
      software.

  The name of the University may not be used to endorse or promote products
  derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED "AS IS" AND WITHOUT ANY EXPRESS OR IMPLIED
  WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR PURPOSE.

  This software was developed by:
     Curtis Smith

     Purdue University
     Engineering Computer Network
     465 Northwestern Avenue
     West Lafayette, Indiana 47907-2035 U.S.A.

  Send all comments, suggestions, or bug reports to:
     software@ecn.purdue.edu

*/

/* Include files */
#include "main.h"
#include "log.h"
#include "syslog.h"
#include "dhcp.h"
#include "wsock.h"

/* WinSock */

/* Indicate if WSAStartup was called */
static WSADATA ws_data;
static BOOL WSockStarted = FALSE;

/* List of items for a server connection */ 
struct WSockSockets {
	char * Name;			/* Name of Socket */
	SOCKET Socket;			/* WinSock Socket */
	BOOL Connected;			/* Is Socket connected and available */
	struct sockaddr_in SockAddress; /* WinSock Address structure */
	BOOL Configured;		/* Was this slot opened via WSockOpen */
};

static struct WSockSockets SyslogSockets[] = {
	{ "LogHost1", INVALID_SOCKET, FALSE },
	{ "LogHost2", INVALID_SOCKET, FALSE },
    { "LogHost3", INVALID_SOCKET, FALSE },
	{ "LogHost4", INVALID_SOCKET, FALSE },
    { "LogHost5", INVALID_SOCKET, FALSE },
    { "LogHost6", INVALID_SOCKET, FALSE },
	{ "LogHostDhcp", INVALID_SOCKET, FALSE }
};

/* Counter of messages that could not be handed to any syslog server */
volatile LONG WSockSendFailures = 0;

// Prototypes //
void ReconnectSocket(int id);
static int WSockSendAll(SOCKET s, const char * buf, int len);

/* Start Winsock access */
int WSockStart()
{
	/* Check to see if started */
	if (WSockStarted == FALSE) {

		/* See if version 2.0 is available */
		if (WSAStartup(MAKEWORD(2, 0), &ws_data)) {
			Log(LOG_ERROR, "Cannot initialize WinSock interface");
			return 1;
		}

		/* Set indicator */
		WSockStarted = TRUE;
	}

	/* Success */
	return 0;
}

/* Stop Winsock access */
void WSockStop()
{
	/* Check to see if started */
	if (WSockStarted) {

		/* Clean up winsock interface */
		WSACleanup();

		/* Reset indicator */
		WSockStarted = FALSE;
	}
}

/* Open connection to syslog */
int WSockOpen(char * loghost, unsigned short port, int ID)
{
	in_addr_t ip;
	int type, protocol;
	DWORD sendTimeout = 5000; /* ms: bound blocking send() so a dead peer cannot stall event delivery */

	/* Convert IP number */
	ip = inet_addr(loghost);
	if (ip == (in_addr_t)(-1)) {
		Log(LOG_ERROR, "Invalid ip address: \"%s\" for %s", loghost, SyslogSockets[ID].Name);
		return 1;
	}

	if (SyslogEnableTcp) {
		type = SOCK_STREAM;
		protocol = IPPROTO_TCP;
	} else {
		type = SOCK_DGRAM;
		protocol = IPPROTO_UDP;
	}

	memset(&SyslogSockets[ID].SockAddress, 0, sizeof(SyslogSockets[ID].SockAddress));
	SyslogSockets[ID].SockAddress.sin_family = AF_INET;
	SyslogSockets[ID].SockAddress.sin_port = htons(port);
	SyslogSockets[ID].SockAddress.sin_addr.s_addr = ip;

	/* Create socket */
	SyslogSockets[ID].Socket = socket(AF_INET, type, protocol);
	if (SyslogSockets[ID].Socket == INVALID_SOCKET) {
		Log(LOG_ERROR|LOG_SYS, "Cannot create a socket for %s (%s:%u)",
			SyslogSockets[ID].Name,
			loghost,
			port);
		return 1;
	}

	setsockopt(SyslogSockets[ID].Socket, SOL_SOCKET, SO_SNDTIMEO,
		(const char *)&sendTimeout, sizeof(sendTimeout));

	/* From here on the slot is owned by the send path (reconnects) */
	SyslogSockets[ID].Configured = TRUE;

	/* A failed connect is not fatal: the socket is kept and WSockSend
	   reconnects later, so an unreachable server cannot block startup */
	ConnectSocket(loghost, port, ID);
	return 0;
}

/* Connect Socket */
int ConnectSocket(char * loghost, unsigned short port, int ID)
{
	int result;

	result = connect(SyslogSockets[ID].Socket,
					 (SOCKADDR*)&SyslogSockets[ID].SockAddress,
					 sizeof(SyslogSockets[ID].SockAddress));

	if (result == SOCKET_ERROR) {
		Log(LOG_ERROR|LOG_SYS,
			"Connecting socket for %s (%s:%u) failed with error %d",
			SyslogSockets[ID].Name,
			loghost,
			port,
			WSAGetLastError());
		SyslogSockets[ID].Connected = FALSE;
		/* Not fatal: WSockSend will keep trying to reconnect */
		return 1;
	}

	/* Success */
	SyslogSockets[ID].Connected = TRUE;
	return 0;
}

// Reconnect a disconnected socket //
void ReconnectSocket(int id)
{
    int result = 0;
	int type, protocol;

	/* A socket whose previous connect() failed cannot be reused reliably;
	   rebuild it before retrying. */
	if (SyslogSockets[id].Socket != INVALID_SOCKET) {
		closesocket(SyslogSockets[id].Socket);
		SyslogSockets[id].Socket = INVALID_SOCKET;
	}
	SyslogSockets[id].Connected = FALSE;

	if (SyslogEnableTcp) {
		type = SOCK_STREAM;
		protocol = IPPROTO_TCP;
	} else {
		type = SOCK_DGRAM;
		protocol = IPPROTO_UDP;
	}

	SyslogSockets[id].Socket = socket(SyslogSockets[id].SockAddress.sin_family, type, protocol);
	if (SyslogSockets[id].Socket == INVALID_SOCKET) {
		Log(LOG_ERROR|LOG_SYS, "Cannot recreate socket for %s", SyslogSockets[id].Name);
		return;
	}

	{
		DWORD sendTimeout = 5000;
		setsockopt(SyslogSockets[id].Socket, SOL_SOCKET, SO_SNDTIMEO,
			(const char *)&sendTimeout, sizeof(sendTimeout));
	}

    result = connect(SyslogSockets[id].Socket,
					 (SOCKADDR*)&SyslogSockets[id].SockAddress,
					 sizeof(SyslogSockets[id].SockAddress));

	if (result == SOCKET_ERROR) {
		/* Logged at most once per outage by the send path */
		return;
	}

    Log(LOG_INFO, "Socket %s reconnected successfully",
        SyslogSockets[id].Name);

	/* Success */
	SyslogSockets[id].Connected = TRUE;
}

/* Close connection */
void WSockClose()
{
	int count = COUNT_OF(SyslogSockets);
	int i;

	for (i = 0; i < count; i++)
	{
		/* Close if open */
		if (SyslogSockets[i].Socket != INVALID_SOCKET)
		{
			shutdown(SyslogSockets[i].Socket, SD_BOTH);
			closesocket(SyslogSockets[i].Socket);
			SyslogSockets[i].Socket = INVALID_SOCKET;
		}
		SyslogSockets[i].Connected = FALSE;
	}
}

/* Send exactly len bytes (send() may write fewer on a stream socket) */
static int WSockSendAll(SOCKET s, const char * buf, int len)
{
	int sent = 0;

	while (sent < len) {
		int n = send(s, buf + sent, len - sent, 0);
		if (n == SOCKET_ERROR || n == 0)
			return 1;
		sent += n;
	}

	return 0;
}

/* Send data to syslog.
   Returns 0 when the message reached at least one server, 1 otherwise. */
int WSockSend(char * message)
{
	int len;
    int i;
    int count = COUNT_OF(SyslogSockets);
	int delivered = 0;

    /* Get message length */
	len = (int) strlen(message);

	for (i = 0; i < count; i++)
	{
	    /* Send to syslog server */
		if (SyslogSockets[i].Configured == FALSE)
			continue;

		if (SyslogSockets[i].Connected == FALSE)
			ReconnectSocket(i);

		if (SyslogSockets[i].Connected == FALSE)
			continue;

		if (WSockSendAll(SyslogSockets[i].Socket, message, len) != 0) {
			int err = WSAGetLastError();

			if (err != WSAEHOSTUNREACH && err != WSAENETUNREACH) {
				// Log the error, but continue operation if possible //
				Log(LOG_ERROR|LOG_SYS, "Cannot send message through socket for %s", SyslogSockets[i].Name);
			}
			SyslogSockets[i].Connected = FALSE;
			continue;
		}

		/* TCP is a byte stream: frame the message with a trailing newline
		   (RFC 6587 non-transparent framing) */
		if (SyslogEnableTcp) {
			if (WSockSendAll(SyslogSockets[i].Socket, "\n", 1) != 0) {
				SyslogSockets[i].Connected = FALSE;
				continue;
			}
		}

		delivered++;
	}

	/* Report failures accurately: nothing reached any server */
	if (delivered == 0) {
		InterlockedIncrement(&WSockSendFailures);
		return 1;
	}

	/* Success */
	return 0;
}
