// Monitor Brightness
// Dan Jackson, 2020.

// NOTE: This is "just enough" to work and needs improvement: better abstractions, correct error handling, general tidying up.

// Generally external monitors (DCC/CI): https://docs.microsoft.com/en-us/windows/win32/api/highlevelmonitorconfigurationapi/nf-highlevelmonitorconfigurationapi-setmonitorbrightness
// Generally internal monitors (WMI): https://docs.microsoft.com/en-us/windows/win32/wmicoreprov/wmisetbrightness-method-in-class-wmimonitorbrightnessmethods?redirectedfrom=MSDN  /  https://stackoverflow.com/questions/47333195/change-brightness-using-wmi  /  https://devblogs.microsoft.com/scripting/use-powershell-to-report-and-set-monitor-brightness/

#define _WIN32_WINNT 0x0600	// 0x0400
#define _WIN32_DCOM

#include <windows.h>
#include <tchar.h>

#include <stdio.h>

#ifdef GetObject	// Otherwise defined as GetObjectW
#undef GetObject
#endif

#include <wbemidl.h>
#include <wbemcli.h>

#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>

// MSC-Specific Pragmas
#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "Dxva2.lib")
#endif

#include "monitor.h"

typedef struct
{
	monitor_t *monitorList;
	monitor_t *lastMonitor;
} enum_state_t;

static wchar_t *variantU16ArrayToString(VARIANT vtProp)
{
	if (vtProp.vt == VT_NULL) return NULL;
	if (vtProp.vt == VT_EMPTY) return NULL;
	if (!(vtProp.vt & VT_ARRAY)) return NULL;
	SAFEARRAY *pSafeArray = vtProp.parray;
	long lLower;
	SafeArrayGetLBound(pSafeArray, 1, &lLower);
	long lUpper;
	SafeArrayGetUBound(pSafeArray, 1, &lUpper);
	size_t length = (size_t)(lUpper - lLower);
	wchar_t *str = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
	memset(str, 0, (length + 1) * sizeof(wchar_t));
	for (long i = lLower; i <= lUpper; i++)
	{
		UINT32 c = 0;
		SafeArrayGetElement(pSafeArray, &i, &c);
		str[i - lLower] = (wchar_t)c;
	}
	return str;
}

static bool WmiSetBrightness(monitor_t *monitor, int brightness)
{
	HRESULT hr = 0;

	// Create locator
	IWbemLocator *locator = NULL;
	hr = CoCreateInstance(&CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (LPVOID *)&locator);
	if (FAILED(hr) || !locator) { fprintf(stderr, "ERROR: Failed CoCreateInstance(CLSID_WbemLocator).\n"); return false; }

	// Connect to WMI
	IWbemServices *services = NULL;
	BSTR bstrResource = SysAllocString(L"ROOT\\WMI"); // "\\\\.\\ROOT\\wmi"
	hr = locator->lpVtbl->ConnectServer(locator, bstrResource, NULL, NULL, NULL, 0, NULL, NULL, &services);
	if (FAILED(hr) || !services) { fprintf(stderr, "ERROR: Failed ConnectServer().\n"); return false; }
	SysFreeString(bstrResource);

	// Proxy security levels
	hr = CoSetProxyBlanket((IUnknown *)services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
	if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed CoSetProxyBlanket().\n"); return false; }

	// Query
	IEnumWbemClassObject *results = NULL;
	// NOTE WMI PATH=WmiMonitorBrightnessMethods.InstanceName="DISPLAY\ACME1234\9&abcdef9&0&UID12345_0"
	BSTR bstrQuery = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");
	BSTR bstrQueryLanguage = SysAllocString(L"WQL");
	hr = services->lpVtbl->ExecQuery(services, bstrQueryLanguage, bstrQuery, WBEM_FLAG_BIDIRECTIONAL, NULL, &results); // WBEM_FLAG_BIDIRECTIONAL or WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY
	SysFreeString(bstrQueryLanguage);
	SysFreeString(bstrQuery);

	if (results != NULL)
	{
		IWbemClassObject *result = NULL;
		ULONG returnedCount = 0;
		while ((hr = results->lpVtbl->Next(results, WBEM_INFINITE, 1, &result, &returnedCount)) == S_OK)
		{
			VARIANT vtInstanceName;
			hr = result->lpVtbl->Get(result, L"InstanceName", 0, &vtInstanceName, 0, 0);
			//_tprintf(TEXT("WMI: instance=%ls\n"), vtInstanceName.bstrVal);

			// If this is the correct monitor...
			if (_tcscmp(monitor->wmiInstance, vtInstanceName.bstrVal) == 0)		// NOTE: This comparison requires a UNICODE build
			{
				BSTR bstrClassName = SysAllocString(L"WmiMonitorBrightnessMethods");
				BSTR bstrMethodName = SysAllocString(L"WmiSetBrightness");

				IWbemClassObject *pClass = NULL;
				hr = services->lpVtbl->GetObject(services, bstrClassName, 0, NULL, &pClass, NULL);
				if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed GetObject().\n"); return false; }

				IWbemClassObject* pInParamsDefinition = NULL;
				hr = pClass->lpVtbl->GetMethod(pClass, bstrMethodName, 0, &pInParamsDefinition, NULL);
				if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed GetMethod().\n"); return false; }

				IWbemClassObject* pClassInstance = NULL;
				hr = pInParamsDefinition->lpVtbl->SpawnInstance(pInParamsDefinition, 0, &pClassInstance);
				if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed SpawnInstance().\n"); return false; }

				VARIANT vtParam1;
				VariantInit(&vtParam1);
				vtParam1.vt = VT_I4;	// uint32
				vtParam1.intVal = 1;	// seconds
				hr = pClassInstance->lpVtbl->Put(pClassInstance, L"Timeout", 0, &vtParam1, CIM_UINT32);
				if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed Put(vtParam1).\n"); return false; }

				VARIANT vtParam2;
				VariantInit(&vtParam2);
				vtParam2.vt = VT_UI1;	// uint8
				vtParam2.intVal = brightness;
				hr = pClassInstance->lpVtbl->Put(pClassInstance, L"Brightness", 0, &vtParam2, CIM_UINT8);
				if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed Put(vtParam2).\n"); return false; }

				// Set "this" pointer to object instance to call method on it
				CIMTYPE type;
				LONG flavor;
				VARIANT vtThis;
				result->lpVtbl->Get(result, L"__RELPATH", 0, &vtThis, &type, &flavor);	// "__RELPATH" / "__PATH"
				// PATH=    WmiMonitorBrightnessMethods.InstanceName="DISPLAY\\XXX1234\\0&abcdef0&0&UID0123456_0"

				// Execute Method
				IWbemClassObject* pOutParams = NULL;
				hr = services->lpVtbl->ExecMethod(services, vtThis.bstrVal, bstrMethodName, 0, NULL, pClassInstance, &pOutParams, NULL);
				if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed ExecMethod() = 0x%08x / 0x%08x\n", (unsigned int)hr, (unsigned int)GetLastError()); return false; } // WBEM_E_INVALID_METHOD_PARAMETERS = 0x8004102F

				VariantClear(&vtThis);
				VariantClear(&vtParam2);
				VariantClear(&vtParam1);

				SysFreeString(bstrMethodName);
				SysFreeString(bstrClassName);
				pClass->lpVtbl->Release(pClass);
				pInParamsDefinition->lpVtbl->Release(pInParamsDefinition);
			}

			result->lpVtbl->Release(result);
		}
	}
	results->lpVtbl->Release(results);

	services->lpVtbl->Release(services);
	locator->lpVtbl->Release(locator);

	return true;
}

static bool WmiUpdateBrightness(monitor_t *monitorList)
{
	HRESULT hr = 0;
	// Create locator
	IWbemLocator *locator = NULL;
	hr = CoCreateInstance(&CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (LPVOID *)&locator);
	if (FAILED(hr) || !locator) { fprintf(stderr, "ERROR: Failed CoCreateInstance(CLSID_WbemLocator).\n"); return false; }

	// Connect to WMI
	IWbemServices *services = NULL;
	BSTR bstrResource = SysAllocString(L"ROOT\\WMI"); // "\\\\.\\ROOT\\wmi"
	hr = locator->lpVtbl->ConnectServer(locator, bstrResource, NULL, NULL, NULL, 0, NULL, NULL, &services);
	if (FAILED(hr) || !services) { fprintf(stderr, "ERROR: Failed ConnectServer().\n"); return false; }
	SysFreeString(bstrResource);

	// Proxy security levels
	hr = CoSetProxyBlanket((IUnknown *)services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
	if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed CoSetProxyBlanket().\n"); return false; }

	// Query
	IEnumWbemClassObject *results = NULL;
	BSTR bstrQuery = SysAllocString(L"SELECT * FROM WmiMonitorBrightness");
	BSTR bstrQueryLanguage = SysAllocString(L"WQL");
	hr = services->lpVtbl->ExecQuery(services, bstrQueryLanguage, bstrQuery, WBEM_FLAG_BIDIRECTIONAL, NULL, &results); // WBEM_FLAG_BIDIRECTIONAL or WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY
	SysFreeString(bstrQueryLanguage);
	SysFreeString(bstrQuery);

	if (results != NULL)
	{
		IWbemClassObject *result = NULL;
		ULONG returnedCount = 0;
		while ((hr = results->lpVtbl->Next(results, WBEM_INFINITE, 1, &result, &returnedCount)) == S_OK)
		{
			VARIANT vtInstanceName;
			hr = result->lpVtbl->Get(result, L"InstanceName", 0, &vtInstanceName, 0, 0);

			VARIANT vtCurrentBrightness;
			hr = result->lpVtbl->Get(result, L"CurrentBrightness", 0, &vtCurrentBrightness, 0, 0);

			VARIANT vtLevels;
			hr = result->lpVtbl->Get(result, L"Levels", 0, &vtLevels, 0, 0);	// count of levels

			VARIANT vtLevel;
			hr = result->lpVtbl->Get(result, L"Level", 0, &vtLevel, 0, 0);		// array of possible levels

			// Locate the instance in the enumerated monitors
			monitor_t *thisMonitor = NULL;
			for (monitor_t *monitor = monitorList; monitor != NULL; monitor = monitor->next)
			{
				if (_tcscmp(monitor->wmiInstance, vtInstanceName.bstrVal) == 0)		// NOTE: This comparison requires a UNICODE build
				{
					thisMonitor = monitor;
				}
			}
			if (thisMonitor)
			{
				thisMonitor->hasWmiBrightness = true;
				thisMonitor->wmiBrightness = vtCurrentBrightness.intVal;
				// At this point, assume minimum of 0 and singly incrementing levels up to maximum
				thisMonitor->wmiMinBrightness = 0;
				thisMonitor->wmiMaxBrightness = thisMonitor->wmiMinBrightness + vtLevels.intVal - 1;
			}

			if (vtLevel.vt != VT_NULL && vtLevel.vt != VT_EMPTY && (vtLevel.vt & VT_ARRAY))
			{
				SAFEARRAY *pSafeArray = vtLevel.parray;

				long lLower;
				SafeArrayGetLBound(pSafeArray, 1, &lLower);
				UINT32 first = 0;
				SafeArrayGetElement(pSafeArray, &lLower, &first);

				long lUpper;
				SafeArrayGetUBound(pSafeArray, 1, &lUpper);
				UINT32 last = 0;
				SafeArrayGetElement(pSafeArray, &lUpper, &last);

				if (last > first && thisMonitor)
				{
					// Take actual minimum and maximum, still assuming there are only single increments
					thisMonitor->wmiMinBrightness = first;
					thisMonitor->wmiMaxBrightness = last;
				}
			}

			//_tprintf(TEXT("WMI: instance=%ls, currentBrightness=%d, levels=%d (%d - %d)\n"), vtInstanceName.bstrVal, vtCurrentBrightness.intVal, vtLevels.intVal, thisMonitor->wmiMinBrightness, thisMonitor->wmiMaxBrightness);
			VariantClear(&vtInstanceName);
			VariantClear(&vtCurrentBrightness);
			VariantClear(&vtLevels);
			VariantClear(&vtLevel);

			result->lpVtbl->Release(result);
		}
		results->lpVtbl->Release(results);
	}

	services->lpVtbl->Release(services);
	locator->lpVtbl->Release(locator);

	return true;
}

static bool EnumWmiMonitors(monitor_t *monitorList)
{
	HRESULT hr = 0;

	// Create locator
	IWbemLocator *locator = NULL;
	hr = CoCreateInstance(&CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (LPVOID *)&locator);
	if (FAILED(hr) || !locator) { fprintf(stderr, "ERROR: Failed CoCreateInstance(CLSID_WbemLocator).\n"); return false; }

	// Connect to WMI
	IWbemServices *services = NULL;
	BSTR bstrResource = SysAllocString(L"ROOT\\WMI"); // "\\\\.\\ROOT\\wmi"
	hr = locator->lpVtbl->ConnectServer(locator, bstrResource, NULL, NULL, NULL, 0, NULL, NULL, &services);
	if (FAILED(hr) || !services) { fprintf(stderr, "ERROR: Failed ConnectServer().\n"); return false; }
	SysFreeString(bstrResource);

	// Proxy security levels
	hr = CoSetProxyBlanket((IUnknown *)services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
	if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed CoSetProxyBlanket().\n"); return false; }

	// Query
	IEnumWbemClassObject *results = NULL;
	BSTR bstrQuery = SysAllocString(L"SELECT * FROM WMIMonitorID");
	BSTR bstrQueryLanguage = SysAllocString(L"WQL");
	hr = services->lpVtbl->ExecQuery(services, bstrQueryLanguage, bstrQuery, WBEM_FLAG_BIDIRECTIONAL, NULL, &results); // WBEM_FLAG_BIDIRECTIONAL or WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY
	SysFreeString(bstrQueryLanguage);
	SysFreeString(bstrQuery);

	if (results != NULL)
	{
		IWbemClassObject *result = NULL;
		ULONG returnedCount = 0;
		while ((hr = results->lpVtbl->Next(results, WBEM_INFINITE, 1, &result, &returnedCount)) == S_OK)
		{
			VARIANT vtInstanceName;
			hr = result->lpVtbl->Get(result, L"InstanceName", 0, &vtInstanceName, 0, 0);

			VARIANT vtManufacturerName;
			hr = result->lpVtbl->Get(result, L"ManufacturerName", 0, &vtManufacturerName, 0, 0);
			wchar_t *manufacturerName = variantU16ArrayToString(vtManufacturerName);
			VariantClear(&vtManufacturerName);

			VARIANT vtUserFriendlyName;
			hr = result->lpVtbl->Get(result, L"UserFriendlyName", 0, &vtUserFriendlyName, 0, 0);
			wchar_t *userFriendlyName = variantU16ArrayToString(vtUserFriendlyName);
			VariantClear(&vtUserFriendlyName);

			// VARIANT vtProductCodeID;
			// hr = result->lpVtbl->Get(result, L"ProductCodeID", 0, &vtProductCodeID, 0, 0);
			// wchar_t *productCodeID = variantU16ArrayToString(vtProductCodeID);
			// VariantClear(&vtProductCodeID);

			// VARIANT vtSerialNumberID;
			// hr = result->lpVtbl->Get(result, L"SerialNumberID", 0, &vtSerialNumberID, 0, 0);
			// wchar_t *serialNumberID = variantU16ArrayToString(vtSerialNumberID);
			// VariantClear(&vtSerialNumberID);

			//VARIANT vtYearOfManufacture;
			//hr = result->lpVtbl->Get(result, L"YearOfManufacture", 0, &vtYearOfManufacture, 0, 0);

			//_tprintf(TEXT("WMI: instance=%s; manufacturer=%s; userFriendly=%s; productCodeID=%s; serialNumberID=%s;\n"), vtInstanceName.bstrVal, manufacturerName, userFriendlyName, productCodeID, serialNumberID);

			// Locate the instance in the enumerated monitors
			monitor_t *thisMonitor = NULL;
			for (monitor_t *monitor = monitorList; monitor != NULL; monitor = monitor->next)
			{
				// ...where we have a non-empty prefix and have not yet located the full instance path...
				if (monitor->wmiInstancePrefix[0] != 0 && monitor->wmiInstance[0] == 0)
				{
					// NOTE: The build has to be UNICODE for direct comparisons with BSTR
					// ...and the prefix matches...
					if (_tcsnccmp(monitor->wmiInstancePrefix, vtInstanceName.bstrVal, _tcslen(monitor->wmiInstancePrefix)) == 0)
					{
						// ...this is the instance
						thisMonitor = monitor;
					}
				}
			}
			if (thisMonitor)
			{
				_tcscpy(thisMonitor->wmiInstance, vtInstanceName.bstrVal);
			}

			VariantClear(&vtInstanceName);
			free(manufacturerName);
			free(userFriendlyName);
			//VariantClear(&vtYearOfManufacture);	

			result->lpVtbl->Release(result);
		}
		results->lpVtbl->Release(results);
	}
	services->lpVtbl->Release(services);
	locator->lpVtbl->Release(locator);
	return true;
}

static void MonitorUpdateBrightness(monitor_t *monitor)
{
	if (monitor->hasBrightness)
	{
		// Get current brightness
		DWORD dwMinimumBrightness = 0, dwCurrentBrightness = 0, dwMaximumBrightness = 0;
		BOOL bResult = GetMonitorBrightness(monitor->physicalMonitor.hPhysicalMonitor, &dwMinimumBrightness, &dwCurrentBrightness, &dwMaximumBrightness);
		//if (!bResult) { _ftprintf(stderr, TEXT("WARNING: GetMonitorBrightness() failed (perhaps the monitor does not support DDC/CI?): 0x%08x\n"), GetLastError()); }
		if (bResult)
		{
			monitor->minBrightness = dwMinimumBrightness;
			monitor->brightness = dwCurrentBrightness;
			monitor->maxBrightness = dwMaximumBrightness;
		}
	}
}

static void MonitorCreate(monitor_t *monitor, MONITORINFOEX monitorInfo, DISPLAY_DEVICE displayDevice, DISPLAY_DEVICE displayDeviceInterface, PHYSICAL_MONITOR physicalMonitor)
{
	memset(monitor, 0, sizeof(monitor_t));
	
	monitor->monitorInfo = monitorInfo;
	monitor->displayDevice = displayDevice;
	monitor->displayDeviceInterface = displayDeviceInterface;
	monitor->physicalMonitor = physicalMonitor;

	DWORD dwMonitorCapabilities = 0, dwSupportedColorTemperatures = 0;
	BOOL bResult = GetMonitorCapabilities(monitor->physicalMonitor.hPhysicalMonitor, &dwMonitorCapabilities, &dwSupportedColorTemperatures);
	// This will fail if DDC/CI not supported (e.g. for internal panels) -- but the WMI interface may still be supported
	//if (!bResult) { _ftprintf(stderr, TEXT("WARNING: GetMonitorCapabilities() failed: 0x%08x\n"), GetLastError()); } // 0x1f = ERROR_GEN_FAILURE
	if (bResult)
	{
		monitor->hasBrightness = (dwMonitorCapabilities & MC_CAPS_BRIGHTNESS) != 0;
		MonitorUpdateBrightness(monitor);
	}

	// monitor->displayDeviceInterface.DeviceID: \\?\DISPLAY#ACME1234#9&abcdef9&0&UID12345#{abcdef01-abcd-abcd-abcd-abcdef012345}
	// WMI instance is: DISPLAY\ACME1234\9&abcdef9&0&UID12345_0
	// WMI PATH: WmiMonitorBrightnessMethods.InstanceName="DISPLAY\ACME1234\9&abcdef9&0&UID12345_0"
	// Determine WMI instance prefix from device interface name
	if (monitor->displayDeviceInterface.DeviceID[0] != '\0')
	{
		TCHAR *src = monitor->displayDeviceInterface.DeviceID;
		// Remove a prefix of "\\?\"
		if (src[0] == TEXT('\\') && src[1] == TEXT('\\') && src[2] == TEXT('?') && src[3] == TEXT('\\')) src += 4;
		TCHAR *dst = monitor->wmiInstancePrefix;
		for (;;)
		{
			TCHAR c = *src++;
			// Hash (#) turns in to backslash (\), unless followed by open curly brace ({) which ends the string
			if (c == TEXT('#'))
			{
				if (*src == TEXT('{')) c = 0;
				else c = TEXT('\\');
			}
			*dst++ = c;
			if (c == 0) break;
		}
	}

}

void MonitorDump(FILE *file, monitor_t *monitor)
{
	_ftprintf(file, TEXT("PHYSICAL_MONITOR: description=%ls\n"), monitor->physicalMonitor.szPhysicalMonitorDescription); // Acme 1234
	_ftprintf(file, TEXT("INFO: hasBrightness=%s\n"), monitor->hasBrightness ? TEXT("true") : TEXT("false"));
	_ftprintf(file, TEXT("INFO: brightness=%d\n"), monitor->brightness);
	_ftprintf(file, TEXT("INFO: minBrightness=%d\n"), monitor->minBrightness);
	_ftprintf(file, TEXT("INFO: maxBrightness=%d\n"), monitor->maxBrightness);

	_ftprintf(file, TEXT("MONITOR: monitorInfo.dwFlags=0x%08x\n"), monitor->monitorInfo.dwFlags);	// MONITORINFOF_PRIMARY=0x00000001
	_ftprintf(file, TEXT("MONITOR: monitorInfo.szDevice=%s\n"), monitor->monitorInfo.szDevice);		// \\.\DISPLAY1

	_ftprintf(file, TEXT("DISPLAY: displayDevice.DeviceName=%s\n"), monitor->displayDevice.DeviceName);		// \\.\DISPLAY1\Monitor0
	_ftprintf(file, TEXT("DISPLAY: displayDevice.DeviceString=%s\n"), monitor->displayDevice.DeviceString);	// Acme 1234
	_ftprintf(file, TEXT("DISPLAY: displayDevice.StateFlags=0x%08x\n"), monitor->displayDevice.StateFlags);	// DISPLAY_DEVICE_ACTIVE=0x00000001, DISPLAY_DEVICE_ATTACHED=0x00000002
	_ftprintf(file, TEXT("DISPLAY: displayDevice.DeviceID=%s\n"), monitor->displayDevice.DeviceID);			// MONITOR\ACME1234\{01234567-0123-0123-0123-0123456789ab}\0001
	_ftprintf(file, TEXT("DISPLAY: displayDevice.DeviceKey=%s\n"), monitor->displayDevice.DeviceKey);		// \Registry\Machine\System\CurrentControlSet\Control\Class\{01234567-0123-0123-0123-0123456789ab}\0001

	_ftprintf(file, TEXT("DISPLAY: displayDeviceInterface.DeviceID=%s\n"), monitor->displayDeviceInterface.DeviceID);	// \\?\DISPLAY#ACME1234#9&abcdef9&0&UID12345#{abcdef01-abcd-abcd-abcd-abcdef012345}

	_ftprintf(file, TEXT("WMI: wmiInstancePrefix=%s\n"), monitor->wmiInstancePrefix);	// prefix (i.e. without trailing "_0" etc): DISPLAY\ACME1234\9&abcdef9&0&UID12345
	_ftprintf(file, TEXT("WMI: wmiInstance=%s\n"), monitor->wmiInstance);				// DISPLAY\ACME1234\9&abcdef9&0&UID12345_0
	_ftprintf(file, TEXT("WMI: wmiHasBrightness=%s\n"), monitor->hasWmiBrightness ? TEXT("true") : TEXT("false"));
	_ftprintf(file, TEXT("WMI: wmiBrightness=%d\n"), monitor->wmiBrightness);
	_ftprintf(file, TEXT("WMI: wmiMinBrightness=%d\n"), monitor->wmiMinBrightness);
	_ftprintf(file, TEXT("WMI: wmiMaxBrightness=%d\n"), monitor->wmiMaxBrightness);
}

static void MonitorDestroy(monitor_t *monitor)
{
	DestroyPhysicalMonitors(1, &monitor->physicalMonitor);
}

bool MonitorHasBrightness(monitor_t *monitor)
{
	return monitor->hasBrightness || monitor->hasWmiBrightness;
}

int MonitorGetBrightness(monitor_t *monitor)
{
	if (monitor->hasBrightness)
	{
		int range = monitor->maxBrightness - monitor->minBrightness;
		if (range <= 0) return 0;
		return (monitor->brightness - monitor->minBrightness) * 100 / range;
	}
	else if (monitor->hasWmiBrightness)
	{
		// TODO: WMI mode should use the enumeration of accepted brightness values (not just assume they're continuous)
		int range = monitor->wmiMaxBrightness - monitor->wmiMinBrightness;
		if (range <= 0) return 0;
		return (monitor->wmiBrightness - monitor->wmiMinBrightness) * 100 / range;
	}
	else
	{
		return 0;
	}
}

void MonitorSetBrightness(monitor_t *monitor, int brightness)
{
	if (monitor->hasBrightness)
	{
		int range = monitor->maxBrightness - monitor->minBrightness;
		if (range <= 0) return;
		int value = brightness * range / 100 + monitor->minBrightness;
		SetMonitorBrightness(monitor->physicalMonitor.hPhysicalMonitor, value);
		monitor->brightness = value;
	}
	else if (monitor->hasWmiBrightness)
	{
		// TODO: WMI mode should use the enumeration of accepted brightness values (not just assume they're continuous)
		int range = monitor->wmiMaxBrightness - monitor->wmiMinBrightness;
		if (range <= 0) return;
		int value = brightness * range / 100 + monitor->wmiMinBrightness;
		WmiSetBrightness(monitor, value);
		monitor->wmiBrightness = value;
	}
}

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hDC, LPRECT lpRect, LPARAM lParam)
{
	//_tprintf(TEXT("===\n"));
	enum_state_t *enumState = (enum_state_t *)lParam;

	DWORD dwNumberOfPhysicalMonitors = 0;
	BOOL bResult = GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &dwNumberOfPhysicalMonitors);
	if (!bResult) { fprintf(stderr, "ERROR: Failed GetNumberOfPhysicalMonitorsFromHMONITOR().\n"); return TRUE; }	// continue anyway
	//	_tprintf(TEXT("MONITOR: Physical monitors=%d\n"), dwNumberOfPhysicalMonitors);

	PHYSICAL_MONITOR *physicalMonitors = (PHYSICAL_MONITOR *)malloc(dwNumberOfPhysicalMonitors * sizeof(PHYSICAL_MONITOR));
	bResult = GetPhysicalMonitorsFromHMONITOR(hMonitor, dwNumberOfPhysicalMonitors, physicalMonitors);
	if (!bResult) { fprintf(stderr, "ERROR: Failed GetPhysicalMonitorsFromHMONITOR().\n"); return TRUE; }	// continue anyway
	for (int i = 0; i < dwNumberOfPhysicalMonitors; i++)
	{
		//_tprintf(TEXT("---\n"));
		monitor_t *newMonitor = (monitor_t *)malloc(sizeof(monitor_t));

		//_tprintf(TEXT("PHYSICAL_MONITOR: description=%ls\n"), physicalMonitors[i].szPhysicalMonitorDescription); // Acme 1234

		// Find which display device(s) are given to this monitor (this could be outside the loop as the same for all physical monitors on this logical monitor)
		MONITORINFOEX monitorInfo;
		memset(&monitorInfo, 0, sizeof(monitorInfo));
		monitorInfo.cbSize = sizeof(monitorInfo);
		bResult = GetMonitorInfo(hMonitor, (LPMONITORINFO)&monitorInfo);
		//_tprintf(TEXT("MONITOR: monitorInfo.dwFlags=0x%08x\n"), monitorInfo.dwFlags);	// MONITORINFOF_PRIMARY=0x00000001
		//_tprintf(TEXT("MONITOR: monitorInfo.szDevice=%s\n"), monitorInfo.szDevice);		// \\.\DISPLAY1

		// ...this might to be the same index as for EnumDisplayDevices?  Perhaps not if multiple video cards?
		// ...we're only using the additional information to link the WMI interface to a monitor.
		DISPLAY_DEVICE displayDevice;
		memset(&displayDevice, 0, sizeof(displayDevice));
		displayDevice.cb = sizeof(displayDevice);
		EnumDisplayDevices(monitorInfo.szDevice, i, &displayDevice, 0);

		DISPLAY_DEVICE displayDeviceInterface;
		memset(&displayDeviceInterface, 0, sizeof(displayDeviceInterface));
		displayDeviceInterface.cb = sizeof(displayDeviceInterface);
		EnumDisplayDevices(monitorInfo.szDevice, i, &displayDeviceInterface, EDD_GET_DEVICE_INTERFACE_NAME);

		// (displayDevice.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
		// _tprintf(TEXT("DISPLAY: displayDevice.DeviceName=%s\n"), displayDevice.DeviceName);		// \\.\DISPLAY1\Monitor0
		// _tprintf(TEXT("DISPLAY: displayDevice.DeviceString=%s\n"), displayDevice.DeviceString);	// Acme 1234
		// _tprintf(TEXT("DISPLAY: displayDevice.StateFlags=0x%08x\n"), displayDevice.StateFlags);	// DISPLAY_DEVICE_ACTIVE=0x00000001, DISPLAY_DEVICE_ATTACHED=0x00000002
		// _tprintf(TEXT("DISPLAY: displayDevice.DeviceID=%s\n"), displayDevice.DeviceID);			// MONITOR\ACME1234\{01234567-0123-0123-0123-0123456789ab}\0001
		// _tprintf(TEXT("DISPLAY: displayDevice.DeviceKey=%s\n"), displayDevice.DeviceKey);		// \Registry\Machine\System\CurrentControlSet\Control\Class\{01234567-0123-0123-0123-0123456789ab}\0001

		// Enum with flag to get interface name
		// _tprintf(TEXT("DISPLAY: displayDeviceInterface.DeviceID=%s\n"), displayDeviceInterface.DeviceID);	// \\?\DISPLAY#ACME1234#9&abcdef9&0&UID12345#{abcdef01-abcd-abcd-abcd-abcdef012345}
		// NOTE: WMI instance is: DISPLAY\ACME1234\9&abcdef9&0&UID12345_0
		// NOTE WMI PATH=WmiMonitorBrightnessMethods.InstanceName="DISPLAY\ACME1234\9&abcdef9&0&UID12345_0"

		// Create monitor object
		MonitorCreate(newMonitor, monitorInfo, displayDevice, displayDeviceInterface, physicalMonitors[i]);

		// Add to end of linked list
		if (enumState->monitorList == NULL)
		{
			enumState->monitorList = newMonitor;
		}

		if (enumState->lastMonitor == NULL)
		{
			newMonitor->index = 0;
		}
		else
		{
			newMonitor->index = enumState->lastMonitor->index + 1;
			enumState->lastMonitor->next = newMonitor;
		}
		enumState->lastMonitor = newMonitor;
	}
	free(physicalMonitors);

	//_tprintf(TEXT("===\n"));

	return TRUE;
}

const wchar_t *MonitorGetDescription(monitor_t *monitor)
{
	return monitor->physicalMonitor.szPhysicalMonitorDescription;
}

monitor_t *MonitorListEnumerate(void)
{
	// Enumerate physical monitors, check for DDC/CI control (typically for external displays).
	enum_state_t state = {0};
	EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)&state);

	// Enumerate WMI brightness controls (typically for internal panels?) and associate with physical display.
_tprintf(TEXT("EnumWmiMonitors...\n"));
	EnumWmiMonitors(state.monitorList);

	// Initial fetch of WMI brightness values
_tprintf(TEXT("WmiUpdateBrightness...\n"));
	WmiUpdateBrightness(state.monitorList);

_tprintf(TEXT("...done\n"));
	return state.monitorList;
}

void MonitorListRefreshBrightness(monitor_t *monitorList)
{
	for (monitor_t *monitor = monitorList; monitor != NULL; monitor = monitor->next)
	{
		MonitorUpdateBrightness(monitor);
	}

	// Don't do any WMI update if there are no supported monitors
	int wmiCount = 0;
	for (monitor_t *monitor = monitorList; monitor != NULL; monitor = monitor->next)
	{
		if (monitor->hasWmiBrightness) wmiCount++;
	}
	if (wmiCount == 0)
	{
		return;
	}
	WmiUpdateBrightness(monitorList);
}

void MonitorListDestroy(monitor_t *monitorList)
{
	for (monitor_t *monitor = monitorList; monitor != NULL; )
	{
		monitor_t *nextMonitor = monitor->next;
		MonitorDestroy(monitor);
		free(monitor);
		monitor = nextMonitor;
	}
}

