
#include "main.h"
#include <malloc.h>
#include <wchar.h>
#include <winevt.h>
#include <winmeta.h>
#include "log.h"
#include "service.h"
#include "syslog.h"
#include "winevent.h"
#include "check.h"
#include "plugin-login.h"

#pragma comment(lib, "delayimp.lib") /* Prevents winevt from loading unless necessary */
#pragma comment(lib, "wevtapi.lib")	 /* New Windows Events logging library for Vista and beyond */

/* Build the compact login summary into the caller-provided buffer.
   Returns the buffer, or NULL when the event should be skipped
   (filtered account or missing fields). */
LPWSTR pluginLogin(EVT_HANDLE hEvent, LPWSTR message, size_t count)
{
	PEVT_VARIANT eventInfo = NULL;
	WCHAR wsGuid[50];

	if (message == NULL || count == 0)
		return NULL;

	eventInfo = GetLoginInfo(hEvent);
	if (!eventInfo) {
		return NULL;
	}

	/* Skip computer accounts ("...$" suffix) and MSOL_* accounts */
	if (eventInfo[0].GuidVal == NULL ||
		eventInfo[1].StringVal == NULL ||
		eventInfo[2].StringVal == NULL ||
		eventInfo[3].StringVal == NULL ||
		EndsWith(eventInfo[1].StringVal, L"$") ||
		StartsWith(eventInfo[1].StringVal, L"MSOL_")){
		free(eventInfo);
		return NULL;
	}
	// Print the values from the System section of the element.
	GuidToString(eventInfo[0].GuidVal, wsGuid, COUNT_OF(wsGuid));

	_snwprintf_s(message, count, _TRUNCATE, L"%s:%s:%s:%s",
		wsGuid,
		 eventInfo[1].StringVal, eventInfo[2].StringVal, eventInfo[3].StringVal
		);

	free(eventInfo);

	return message;
}


/* Get specific values from an event */
PEVT_VARIANT GetLoginInfo(EVT_HANDLE hEvent)
{
	LPWSTR ppValues[] = {
		L"Event/EventData/Data[@Name='LogonGuid']",
		L"Event/EventData/Data[@Name='TargetUserName']",
		L"Event/EventData/Data[@Name='WorkstationName']",
		L"Event/EventData/Data[@Name='IpAddress']" };

	return WinEventRenderValues(hEvent, (LPCWSTR*)ppValues, COUNT_OF(ppValues));
}

WCHAR * GuidToString(GUID * guid, WCHAR * out, size_t count)
{
	if (out == NULL || count == 0)
		return NULL;

	if (guid == NULL){
		_snwprintf_s(out, count, _TRUNCATE, L"00000000-0000-0000-0000-000000000000");
		return out;
	}

	_snwprintf_s(out, count, _TRUNCATE, L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		guid->Data1,
		guid->Data2,
		guid->Data3,
		guid->Data4[0],
		guid->Data4[1],
		guid->Data4[2],
		guid->Data4[3],
		guid->Data4[4],
		guid->Data4[5],
		guid->Data4[6],
		guid->Data4[7]
		);

	return out;
}
