/*
  Copyright (c) 2009, Rochester Institute of Technology
  All rights reserved.

  Redistribution and use in source and binary forms are permitted provided
  that:

  (1) source distributions retain this entire copyright notice and comment,
      and
  (2) distributions including binaries display the following acknowledgement:

         "This product includes software developed by Rochester Institute of Technology."

      in the documentation or other materials provided with the distribution
      and in all advertising materials mentioning features or use of this
      software.

  The name of the University may not be used to endorse or promote products
  derived from this software without specific prior written permission.

  This software contains code taken from the Eventlog to Syslog service
  developed by Curtis Smith of Purdue University.

  THIS SOFTWARE IS PROVIDED "AS IS" AND WITHOUT ANY EXPRESS OR IMPLIED
  WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR PURPOSE.

  This software was developed by:
     Sherwin Faria

     Rochester Institute of Technology
     Information and Technology Services
     1 Lomb Memorial Drive, Bldg 10
     Rochester, NY 14623 U.S.A.

  Send all comments, suggestions, or bug reports to:
     sherwin.faria@gmail.com

*/
#include "main.h"
#include <malloc.h>
#include <wchar.h>
#include <winevt.h>
#include <winmeta.h>
#include "log.h"
#include "service.h"
#include "syslog.h"
#include "winevent.h"
#include "plugin-login.h"
#include "check.h"

#pragma comment(lib, "delayimp.lib") /* Prevents winevt from loading unless necessary */
#pragma comment(lib, "wevtapi.lib")	 /* New Windows Events logging library for Vista and beyond */

/* Prototypes */
DWORD ProcessEvent(EVT_HANDLE hEvent, PVOID pContext );
DWORD WINAPI WinEventCallback(EVT_SUBSCRIBE_NOTIFY_ACTION action, PVOID pContext, EVT_HANDLE hEvent);

/* Number of eventlogs */
#define WIN_EVENTLOG_SZ		32

/* Eventlog descriptor */
struct WinEventlog {
	WCHAR name[WIN_EVENTLOG_NAME_SZ];	/* Name of eventlog		*/
	HANDLE handle;					/* Handle to eventlog	*/
	int recnum;					/* Next record number		*/
};

int		   subscriptions = 0;
EVT_HANDLE WinEventSub[MAX_SUBSCRIPTIONS];

/* Counter of events whose message text could not be formatted */
volatile LONG WinEventFormatFailures = 0;

/* Subscribe to new events */
DWORD WinEventSubscribe(XPathList * xpathQueries, int queryCount)
{
    LPWSTR error_msg = NULL;
    WCHAR * pQueryL;
    DWORD used;
	DWORD status = ERROR_SUCCESS;
	XPathList * iter;

    pQueryL = (WCHAR*)malloc(QUERY_LIST_SZ);
	if (pQueryL == NULL) {
		Log(LOG_ERROR|LOG_SYS, "Cannot allocate query buffer");
		return ERR_FAIL;
	}

	error_msg = (LPWSTR)malloc(SYSLOG_DEF_SZ*sizeof(WCHAR));
	if (error_msg == NULL) {
		Log(LOG_ERROR|LOG_SYS, "Cannot allocate error buffer");
		free(pQueryL);
		return ERR_FAIL;
	}

	for (iter = xpathQueries; iter != NULL; iter = iter->next) {
		EVT_HANDLE subscription;

		if(!CreateQueryString(pQueryL, iter))
			continue;

		subscription = EvtSubscribe(NULL, NULL, NULL, pQueryL, NULL, iter->plugin,
									 WinEventCallback,
									 EvtSubscribeToFutureEvents);

		if (subscription == NULL)
		{
			DWORD error = GetLastError();

			if (ERROR_EVT_CHANNEL_NOT_FOUND == error)
				Log(LOG_WARNING, "Channel %s was not found.\n", iter->source);
			else if (ERROR_EVT_INVALID_QUERY == error)
			{
				Log(LOG_ERROR, "The query \"%S\" is not valid.\n", pQueryL);

				if (EvtGetExtendedStatus(SYSLOG_DEF_SZ, error_msg, &used) == ERROR_SUCCESS)
					Log(LOG_ERROR, "%S", error_msg);
			}
			else
				Log(LOG_ERROR | LOG_SYS, "EvtSubscribe failed with %lu.\n", error);

			/* Keep the subscriptions that succeeded; report the failure */
			status = ERR_FAIL;
			continue;
		}

		if (subscriptions >= MAX_SUBSCRIPTIONS) {
			Log(LOG_ERROR, "Subscription limit (%d) reached; extra subscription closed", MAX_SUBSCRIPTIONS);
			EvtClose(subscription);
			status = ERR_FAIL;
			break;
		}

		WinEventSub[subscriptions++] = subscription;
	}

	free(error_msg);
	free(pQueryL);
    return status;
}

/* Create an XML query string for subscription */
boolean CreateQueryString(WCHAR * pQueryL, XPathList * xpathQueries)
{
    WCHAR query[QUERY_SZ];
    int i = 0;
    
    wcscpy_s(pQueryL, QUERY_LIST_SZ, L"<QueryList>");

	if (xpathQueries->source == NULL) {
		return FALSE;
	}

			swprintf_s(query, QUERY_SZ,
				L"<Query Id=\"%i\" Path=\"%S\">%S</Query>",
				i++,
				xpathQueries->source,
				xpathQueries->query
				);

			wcscat_s(pQueryL, QUERY_LIST_SZ, query);
    
    wcscat_s(pQueryL, QUERY_LIST_SZ, L"</QueryList>");
	return TRUE;
}

/* Cancel the subscription */
void WinEventCancelSubscribes()
{
	/* subscriptions is the count of valid handles (indices 0..subscriptions-1) */
	while (subscriptions > 0){
		EVT_HANDLE handle = WinEventSub[--subscriptions];
		WinEventSub[subscriptions] = NULL;
		if (handle != NULL)
			EvtClose(handle);
	}
}

/* This function is called whenever a matching event is triggered */
DWORD WINAPI WinEventCallback(EVT_SUBSCRIBE_NOTIFY_ACTION action, PVOID context, EVT_HANDLE hEvent)
{
    DWORD status = ERROR_SUCCESS;

    switch(action)
    {
        case EvtSubscribeActionError:
            if (ERROR_EVT_QUERY_RESULT_STALE == (DWORD)hEvent)
            {
                Log(LOG_WARNING, "The subscription callback was notified that event records are missing.");
            }
            else
            {
                Log(LOG_WARNING | LOG_SYS, "The subscription callback received the following Win32 error: %lu", (DWORD)hEvent);
            }
            break;

        case EvtSubscribeActionDeliver:
            status = ProcessEvent(hEvent, context);
            break;

        default:
            Log(LOG_WARNING, "SubscriptionCallback: Unknown action.");
    }

    if (status == ERR_FAIL)
    {
		Log(LOG_ERROR | LOG_SYS, "Error sending log message");

		/* Only signal shutdown here: subscription handles are destroyed by
		   the management thread in MainLoop, never from a callback thread. */
		ServiceSignalStop();
    }

    return status; // The service ignores the returned status.
}

/* Render a set of event property values into a heap-allocated buffer.
   Handles the grow-and-retry dance of EvtRender safely. Returns NULL on
   failure; the caller must free() the returned buffer. */
PEVT_VARIANT WinEventRenderValues(EVT_HANDLE hEvent, LPCWSTR * values, DWORD count)
{
	EVT_HANDLE hContext = NULL;
	PEVT_VARIANT pRenderedEvents = NULL;
	DWORD dwReturned = 0;
	DWORD dwBufferSize = (256*sizeof(LPWSTR)*count);
	DWORD dwValuesCount = 0;
	BOOL ok = FALSE;

	/* Create the context to use for EvtRender */
	hContext = EvtCreateRenderContext(count, values, EvtRenderContextValues);
	if (NULL == hContext) {
		Log(LOG_ERROR|LOG_SYS, "EvtCreateRenderContext failed");
		goto cleanup;
	}

	pRenderedEvents = (PEVT_VARIANT)malloc(dwBufferSize);
	if (pRenderedEvents == NULL) {
		Log(LOG_ERROR|LOG_SYS, "malloc failed for event render buffer");
		goto cleanup;
	}

	/* Use EvtRender to capture the values from the Event */
	/* Log Errors to the event log if things go wrong */
	if (!EvtRender(hContext, hEvent, EvtRenderEventValues, dwBufferSize, pRenderedEvents, &dwReturned, &dwValuesCount)) {
		PEVT_VARIANT pNewBuffer;

		if (ERROR_INSUFFICIENT_BUFFER != GetLastError()) {
			if (LogInteractive)
				printf("Error Rendering Event");
			goto cleanup;
		}

		/* Grow the buffer to the reported size and render again */
		dwBufferSize = dwReturned;
		pNewBuffer = (PEVT_VARIANT)realloc(pRenderedEvents, dwBufferSize);
		if (pNewBuffer == NULL) {
			Log(LOG_ERROR|LOG_SYS, "realloc failed for event render buffer");
			goto cleanup;
		}
		pRenderedEvents = pNewBuffer;

		if (!EvtRender(hContext, hEvent, EvtRenderEventValues, dwBufferSize, pRenderedEvents, &dwReturned, &dwValuesCount)) {
			if (LogInteractive)
				printf("Error Rendering Event");
			goto cleanup;
		}
	}

	if (dwValuesCount != count) {
		if (LogInteractive)
			printf("Error Rendering Event");
		goto cleanup;
	}

	ok = TRUE;

cleanup:
	if (hContext)
		EvtClose(hContext);

	if (!ok) {
		if (pRenderedEvents != NULL)
			free(pRenderedEvents);
		pRenderedEvents = NULL;
	}

	return pRenderedEvents;
}

/* Get specific values from an event */
PEVT_VARIANT GetEventInfo(EVT_HANDLE hEvent)
{
	LPWSTR ppValues[] = {L"Event/System/Provider/@Name",
						 L"Event/System/TimeCreated/@SystemTime",
						 L"Event/System/EventID",
						 L"Event/System/Level",
						 L"Event/System/Keywords",
						 L"Event/System/Channel",
						 L"Event/System/EventRecordID" };

	return WinEventRenderValues(hEvent, (LPCWSTR*)ppValues, COUNT_OF(ppValues));
}

/* Render the raw event XML. Returns a heap string the caller must free(). */
static LPWSTR GetEventXmlString(EVT_HANDLE hEvent)
{
	LPWSTR pBuffer = NULL;
	DWORD dwBufferSize = 0;
	DWORD dwBufferUsed = 0;
	DWORD dwValuesCount = 0;

	/* First call determines the required size (in bytes) */
	if (!EvtRender(NULL, hEvent, EvtRenderEventXml, 0, NULL, &dwBufferUsed, &dwValuesCount)) {
		if (ERROR_INSUFFICIENT_BUFFER != GetLastError())
			return NULL;
	}

	if (dwBufferUsed == 0)
		return NULL;

	dwBufferSize = dwBufferUsed;
	pBuffer = (LPWSTR)malloc(dwBufferSize);
	if (pBuffer == NULL) {
		Log(LOG_ERROR|LOG_SYS, "malloc failed for event XML buffer");
		return NULL;
	}

	if (!EvtRender(NULL, hEvent, EvtRenderEventXml, dwBufferSize, pBuffer, &dwBufferUsed, &dwValuesCount)) {
		free(pBuffer);
		return NULL;
	}

	return pBuffer;
}

/* Gets the specified message string from the event. If the event does not
   contain the specified message, the function returns NULL. */
LPWSTR GetMessageString(EVT_HANDLE hMetadata, EVT_HANDLE hEvent)
{
	LPWSTR pBuffer = NULL;
	DWORD dwBufferSize = 0;
	DWORD dwBufferUsed = 0;
	DWORD status = 0;

	/* Get the message string from the provider */
	EvtFormatMessage(hMetadata, hEvent, 0, 0, NULL, EvtFormatMessageEvent, dwBufferSize, pBuffer, &dwBufferUsed);
    
	/* Ensure the call succeeded */
	/* If buffer was not large enough realloc it */
	status = GetLastError();
	if (ERROR_INSUFFICIENT_BUFFER == status) {
		dwBufferSize = dwBufferUsed;

		pBuffer = (LPWSTR)malloc(dwBufferSize * sizeof(WCHAR));

		/* Once we have realloc'd the buffer try to grab the message string again */
		if (pBuffer)
			EvtFormatMessage(hMetadata, hEvent, 0, 0, NULL, EvtFormatMessageEvent, dwBufferSize, pBuffer, &dwBufferUsed);
		else {
			Log(LOG_ERROR|LOG_SYS, "EvtFormatMessage: malloc failed");
			return NULL;
		}
	}
	else if (ERROR_EVT_MESSAGE_NOT_FOUND == status || ERROR_EVT_MESSAGE_ID_NOT_FOUND == status) {
		if (pBuffer)
			free(pBuffer);
		return NULL;
	}
	else {
		if (LogInteractive)
			printf("EvtFormatMessage failed : could not get message string\n");
		if (pBuffer)
			free(pBuffer);
		return NULL;
	}

	/* Success */
	return pBuffer;
}

/* Process a given event */
DWORD ProcessEvent(EVT_HANDLE hEvent, PVOID context)
{
    EVT_HANDLE hProviderMetadata = NULL;
	PEVT_VARIANT eventInfo = NULL;
    LPWSTR pwsMessage = NULL;
	LPWSTR pwszPublisherName = NULL;
	ULONGLONG eventTime;
	ULONGLONG keyword;
    DWORD status = ERROR_SUCCESS;
	int event_id = 0;
	int winlevel = 0;
	int level = 0;

	WCHAR source[SOURCE_SZ];
	WCHAR hostname[HOSTNAME_SZ];
	WCHAR tstamp_buf[32];
	WCHAR plugin_msg[SYSLOG_DEF_SZ];
	WCHAR collapsed_msg[SYSLOG_DEF_SZ];
	WCHAR * formatted_string = NULL;
	WCHAR * tstamp = NULL;
	WCHAR * index = NULL;
	WCHAR defmsg[ERRMSG_SZ];
	WCHAR tstamped_message[SYSLOG_DEF_SZ];


    /* Get and store the publishers new Windows Events name */
	eventInfo = GetEventInfo(hEvent);
	if (!eventInfo) {
		return ERR_CONTINUE;
	}
	pwszPublisherName = (LPWSTR)eventInfo[0].StringVal;
	eventTime = eventInfo[1].FileTimeVal;

	/* Read the EventID through the type the API actually returned */
	switch (eventInfo[2].Type) {
	case EvtVarTypeUInt16:
		event_id = (int)eventInfo[2].UInt16Val;
		break;
	case EvtVarTypeUInt32:
		event_id = (int)eventInfo[2].UInt32Val;
		break;
	case EvtVarTypeInt16:
		event_id = (int)eventInfo[2].Int16Val;
		break;
	case EvtVarTypeInt32:
		event_id = (int)eventInfo[2].Int32Val;
		break;
	default:
		event_id = 0;
		break;
	}

	/* Check for the "Microsoft-Windows-" prefix in the publisher name */
	/* and remove it if found. Saves 18 characters in the message */
	if (pwszPublisherName == NULL)
		wcsncpy_s(source, COUNT_OF(source), L"UnknownSource", _TRUNCATE);
	else if(wcsncmp(pwszPublisherName, L"Microsoft-Windows-", 18) == 0)
		wcsncpy_s(source, COUNT_OF(source), pwszPublisherName+18, _TRUNCATE);
	else
		wcsncpy_s(source, COUNT_OF(source), pwszPublisherName, _TRUNCATE);

	/* Check Event Info Against Ignore List */
	if (WIgnoreSyslogEvent(IgnoredEvents, source, event_id)) {
		free(eventInfo);
		return ERR_CONTINUE;
	}

	/* Format Event Timestamp */
	if (WinEventTimeToString(eventTime, tstamp_buf, COUNT_OF(tstamp_buf)) == NULL)
		tstamp = L"TIME_ERROR";
	else
		tstamp = tstamp_buf;

	/* Add hostname for RFC compliance (RFC 3164) */
	if (ProgramUseIPAddress == TRUE) {
		_snwprintf_s(hostname, HOSTNAME_SZ, _TRUNCATE, L"%S", ProgramHostName);
	} else {
		if (ExpandEnvironmentStringsW(L"%COMPUTERNAME%", hostname, COUNT_OF(hostname)) == 0) {
			wcscpy_s(hostname, COUNT_OF(hostname), L"HOSTNAME_ERR");
			Log(LOG_ERROR|LOG_SYS, "Cannot expand %COMPUTERNAME%");
		}
    }

	/* replace every space in source by underscores */
	index = source;
	while( *index ) {
		if( *index == L' ' ) {
			*index = L'_';
		}
		index++;
	}

	/* Add Timestamp and hostname then format source & event ID for consistency with Event Viewer */
    if(SyslogIncludeTag)
    {
        _snwprintf_s(tstamped_message, COUNT_OF(tstamped_message), _TRUNCATE, L"%s %s %S: %s: %i: ",
            tstamp,
            hostname,
            SyslogTag,
            source,
            event_id
        );
    }
    else
    {
        _snwprintf_s(tstamped_message, COUNT_OF(tstamped_message), _TRUNCATE, L"%s %s %s: %i: ",
            tstamp,
            hostname,
            source,
            event_id
        );
    }

	/********************************************/
	/*         GENERATE EVENT MESSAGE           */
	/********************************************/
	if (context == NULL){
		winlevel = (int)eventInfo[3].ByteVal;
		/* Select syslog level */
		switch (winlevel) {
		case WINEVENT_CRITICAL_LEVEL:
			level = LOG_ERROR;
			break;
		case WINEVENT_ERROR_LEVEL:
			level = LOG_ERROR;
			break;
		case WINEVENT_WARNING_LEVEL:
			level = LOG_WARNING;
			break;
		default:
			level = LOG_INFO;
			break;
		}


		/* Get the handle to the provider's metadata that contains the message strings. */
		if (pwszPublisherName != NULL)
			hProviderMetadata = EvtOpenPublisherMetadata(NULL, pwszPublisherName, NULL, 0, 0);

		/* Get the message string from the event */
		if (hProviderMetadata != NULL)
			pwsMessage = GetMessageString(hProviderMetadata, hEvent);

		if (pwsMessage == NULL) {
			/* A missing provider/message DLL does not invalidate the event.
			   Count the failure and fall back to the raw event XML. */
			InterlockedIncrement(&WinEventFormatFailures);
			Log(level | LOG_SYS, "%i:%S: Error getting message string for event DETAILS: Publisher: %S EventID: %i", event_id, source, source, event_id);
			pwsMessage = GetEventXmlString(hEvent);
		}

		/* Get string and strip whitespace */
		if (pwsMessage != NULL)
			formatted_string = CollapseExpandMessageW(pwsMessage, collapsed_msg, COUNT_OF(collapsed_msg));

		/* Create a default message if resources or formatting didn't work */
		if (formatted_string == NULL) {
			/* Last resort: keep the identifying fields so the event is not lost */
			LPCWSTR channel = (EvtVarTypeString == eventInfo[5].Type && eventInfo[5].StringVal != NULL) ? eventInfo[5].StringVal : L"?";
			ULONGLONG record_id = (EvtVarTypeUInt64 == eventInfo[6].Type) ? eventInfo[6].UInt64Val : 0;

			if(SyslogIncludeTag)
			{
				_snwprintf_s(defmsg, COUNT_OF(defmsg), _TRUNCATE,
					L"%S: (Channel: %s, Provider: %s, EventID: %i, RecordID: %I64u)",
					SyslogTag,
					channel,
					source,
					event_id,
					record_id
				);
			}
			else
			{
				_snwprintf_s(defmsg, COUNT_OF(defmsg), _TRUNCATE,
					L"(Channel: %s, Provider: %s, EventID: %i, RecordID: %I64u)",
					channel,
					source,
					event_id,
					record_id
				);
			}
			formatted_string = defmsg;
		}
	}
	else {
		/*if (LogInteractive){
			Log(LOG_INFO, "Run login plugin");
		}*/
		formatted_string = pluginLogin(hEvent, plugin_msg, COUNT_OF(plugin_msg));

		/*if (LogInteractive) {
			Log(LOG_INFO, "Plugin returned: %ls", formatted_string);
		}*/
		if (formatted_string == NULL){
			free(eventInfo);

			/* The event handle belongs to the Event Log service (see
			   EVT_SUBSCRIBE_CALLBACK) and must not be closed here. */
			return ERR_CONTINUE;
		}

	}


	/* Get Event Error Level. In the case of Security Events,
	 * set Failures to Error instead of notice using the
	 * keyword attribute
	 */
	keyword = (EvtVarTypeNull == eventInfo[4].Type) ? 0 : eventInfo[4].UInt64Val;
	if ((keyword & WINEVENT_KEYWORD_AUDIT_FAILURE) != 0) {
        // Add AUDIT_FAILURE message for better parsing
        wcsncat_s(tstamped_message, COUNT_OF(tstamped_message), L"AUDIT_FAILURE ", _TRUNCATE);
		winlevel = WINEVENT_ERROR_LEVEL;
    }
	else
		winlevel = (int)eventInfo[3].ByteVal;

	/* Select syslog level */
	switch (winlevel) {
		case WINEVENT_CRITICAL_LEVEL:
			level = SYSLOG_BUILD(SyslogFacility, SYSLOG_CRIT);
			break;		
		case WINEVENT_ERROR_LEVEL:
			level = SYSLOG_BUILD(SyslogFacility, SYSLOG_ERR);
			break;
		case WINEVENT_WARNING_LEVEL:
			level = SYSLOG_BUILD(SyslogFacility, SYSLOG_WARNING);
			break;
		case WINEVENT_INFORMATION_LEVEL:
			level = SYSLOG_BUILD(SyslogFacility, SYSLOG_NOTICE);
			break;
		case WINEVENT_AUDIT_LEVEL:
            wcsncat_s(tstamped_message, COUNT_OF(tstamped_message), L"AUDIT_SUCCESS ", _TRUNCATE);
			level = SYSLOG_BUILD(SyslogFacility, SYSLOG_NOTICE);
			break;
		case WINEVENT_VERBOSE_LEVEL:
			level = SYSLOG_BUILD(SyslogFacility, SYSLOG_DEBUG);
			break;

		/* Everything else */
		default:
			level = SYSLOG_BUILD(SyslogFacility, SYSLOG_NOTICE);
			break;
	}

	/* Combine the message strings */
	wcsncat_s(tstamped_message, COUNT_OF(tstamped_message), formatted_string, _TRUNCATE);

	/* Send the event to the Syslog Server */
	/* Making sure it is severe enough to be logged */
	/* Delivery failures are counted in WSockSend; they must not stop the
	   service (a persistent send queue is planned for v5.1). */
	if (SyslogLogLevel == 0 || (SyslogLogLevel >= (DWORD)winlevel && winlevel > 0))
		SyslogSendW(tstamped_message, level);

	/* Cleanup memory and open handles */
	if(pwsMessage)
		free(pwsMessage);
	if(eventInfo)
		free(eventInfo);

	if (hProviderMetadata)
		EvtClose(hProviderMetadata);
	/* Note: hEvent is owned by the Event Log service (see EVT_SUBSCRIBE_CALLBACK) */

	return status;
}

/* Format Timestamp from EventLog into the caller-provided buffer */
WCHAR * WinEventTimeToString(ULONGLONG ulongTime, WCHAR * result, size_t resultCount)
{
	SYSTEMTIME sysTime;
	FILETIME fTime, lfTime;
	ULARGE_INTEGER ulargeTime;
	struct tm tm_struct;
	WCHAR buffer[32];

	if (result == NULL || resultCount < 17)
		return NULL;

	memset(&tm_struct, 0, sizeof(tm_struct));

	/* Convert from ULONGLONG to usable FILETIME value */
	ulargeTime.QuadPart = ulongTime;
	
	fTime.dwLowDateTime = ulargeTime.LowPart;
	fTime.dwHighDateTime = ulargeTime.HighPart;

	/* Adjust time value to reflect current timezone */
	/* then convert to a SYSTEMTIME */
	if (FileTimeToLocalFileTime(&fTime, &lfTime) == 0) {
		Log(LOG_ERROR|LOG_SYS,"Error formatting event time to local time");
		return NULL;
	}
	if (FileTimeToSystemTime(&lfTime, &sysTime) == 0) {
		Log(LOG_ERROR|LOG_SYS,"Error formatting event time to system time");
		return NULL;
	}

	/* Convert SYSTEMTIME to tm */
	tm_struct.tm_year = sysTime.wYear - 1900;
	tm_struct.tm_mon  = sysTime.wMonth - 1;
	tm_struct.tm_mday = sysTime.wDay;
	tm_struct.tm_hour = sysTime.wHour;
	tm_struct.tm_wday = sysTime.wDayOfWeek;
	tm_struct.tm_min  = sysTime.wMinute;
	tm_struct.tm_sec  = sysTime.wSecond;
	
	/* Format timestamp string */
	wcsftime(buffer, COUNT_OF(buffer), L"%b %d %H:%M:%S", &tm_struct);
	if (buffer[4] == L'0') /* Replace leading zero with a space for */
		buffer[4] = L' ';  /* single digit days so we comply with the RFC */

	wcsncpy_s(result, resultCount, buffer, _TRUNCATE);
	
	return result;
}