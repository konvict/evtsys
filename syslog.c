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

// Include files //
#include "main.h"
#include "syslog.h"
#include "wsock.h"
#include "dhcp.h"

// syslog //

// Application data configuration //
char SyslogLogHosts[(SYSLOG_HOST_SZ+1)*6];
char SyslogLogHost1[SYSLOG_HOST_SZ+1];
char SyslogLogHost2[SYSLOG_HOST_SZ+1];
char SyslogLogHost3[SYSLOG_HOST_SZ+1];
char SyslogLogHost4[SYSLOG_HOST_SZ+1];
char SyslogLogHost5[SYSLOG_HOST_SZ+1];
char SyslogLogHost6[SYSLOG_HOST_SZ+1];
char SyslogLogHostDhcp[SYSLOG_HOST_SZ+1];
char SyslogConfigFile[MAX_CONFIG_FNAME+1];
char SyslogTag[SYSLOG_TAG_SZ+1];
DWORD SyslogIncludeTag = FALSE;
DWORD SyslogPort = SYSLOG_DEF_PORT;
DWORD SyslogFacility = SYSLOG_DEF_FAC;
DWORD SyslogStatusInterval = SYSLOG_DEF_INTERVAL;
DWORD SyslogQueryDhcp = FALSE;
DWORD SyslogLogLevel = SYSLOG_DEF_LogLevel;
DWORD SyslogIncludeOnly = FALSE;
DWORD SyslogMessageSize = SYSLOG_DEF_SZ;
DWORD SyslogEnableTcp = FALSE;

// Open syslog connection //
int SyslogOpen()
{
	int opened = 0;
	int failed = 0;

	// Query the dhcp if enabled //
	if(SyslogQueryDhcp && !DHCPQuery()) {
		if (WSockOpen(SyslogLogHostDhcp, (unsigned short)SyslogPort, LOG_HOST_DHCP) == 0)
			opened++;
		else
			failed++;
	}

	// Start network connections: one bad host must not block the others //
	if (SyslogLogHost1[0] != '\0') {
		if (WSockOpen(SyslogLogHost1, (unsigned short)SyslogPort, LOG_HOST1) == 0)
			opened++;
		else
			failed++;
	}

	if (SyslogLogHost2[0] != '\0') {
		if (WSockOpen(SyslogLogHost2, (unsigned short)SyslogPort, LOG_HOST2) == 0)
			opened++;
		else
			failed++;
	}

	if (SyslogLogHost3[0] != '\0') {
		if (WSockOpen(SyslogLogHost3, (unsigned short)SyslogPort, LOG_HOST3) == 0)
			opened++;
		else
			failed++;
	}

	if (SyslogLogHost4[0] != '\0') {
		if (WSockOpen(SyslogLogHost4, (unsigned short)SyslogPort, LOG_HOST4) == 0)
			opened++;
		else
			failed++;
	}

    if (SyslogLogHost5[0] != '\0') {
		if (WSockOpen(SyslogLogHost5, (unsigned short)SyslogPort, LOG_HOST5) == 0)
			opened++;
		else
			failed++;
	}

    if (SyslogLogHost6[0] != '\0') {
		if (WSockOpen(SyslogLogHost6, (unsigned short)SyslogPort, LOG_HOST6) == 0)
			opened++;
		else
			failed++;
	}

    // Succeed as long as at least one target socket was opened //
    // (an unreachable server only marks its slot for reconnection) //
	if (opened > 0 || failed == 0)
		return 0;

	return 1;
}

// Close syslog connection //
void SyslogClose()
{
	WSockClose();
}

// Send a message to the syslog server //
int SyslogSend(char * message, int level)
{
	char error_message[SYSLOG_DEF_SZ];
	WCHAR utf16_message[SYSLOG_DEF_SZ];
	/* UTF-8 needs up to 3 bytes per UTF-16 code unit for CJK text */
	char utf8_message[SYSLOG_DEF_SZ * 3 + 32];

	// Write priority level //
	_snprintf_s(error_message, sizeof(error_message), _TRUNCATE,
		"<%d>%s",
		level,
		message
	);

	// convert from ansi/cp850 codepage (local system codepage) to utf8, widely used codepage on unix systems //
	if (MultiByteToWideChar(CP_ACP, 0, error_message, -1, utf16_message, SYSLOG_DEF_SZ) == 0) {
		// Fall back to a plain ASCII notice so the event is not lost silently //
		_snprintf_s(utf8_message, sizeof(utf8_message), _TRUNCATE,
			"<%d>evtsys: message conversion failed", level);
		return WSockSend(utf8_message);
	}

	if (WideCharToMultiByte(CP_UTF8, 0, utf16_message, -1, utf8_message, (int)sizeof(utf8_message), NULL, NULL) == 0) {
		_snprintf_s(utf8_message, sizeof(utf8_message), _TRUNCATE,
			"<%d>evtsys: message conversion failed", level);
	}

	// Send result to syslog server //
	return WSockSend(utf8_message);
}

// Send a message to the syslog server //
int SyslogSendW(WCHAR * message, int level)
{
	WCHAR utf16_message[SYSLOG_DEF_SZ];
	/* UTF-8 needs up to 3 bytes per UTF-16 code unit for CJK text */
	char utf8_message[SYSLOG_DEF_SZ * 3 + 32];


	// Write priority level //
	_snwprintf_s(utf16_message, COUNT_OF(utf16_message), _TRUNCATE,
		L"<%d>%s",
		level,
		message
	);

	// convert to utf8, widely used codepage on unix systems //
	if (WideCharToMultiByte(CP_UTF8, 0, utf16_message, -1, utf8_message, (int)sizeof(utf8_message), NULL, NULL) == 0) {
		// Fall back to a plain ASCII notice so the event is not lost silently //
		_snprintf_s(utf8_message, sizeof(utf8_message), _TRUNCATE,
			"<%d>evtsys: message conversion failed", level);
	}

	// Send result to syslog server //
	return WSockSend(utf8_message);
}