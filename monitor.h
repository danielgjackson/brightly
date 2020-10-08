// Monitor Brightness
// Dan Jackson, 2020.

#ifndef _MONITOR_H
#define _MONITOR_H

#include <windows.h>
#include <highlevelmonitorconfigurationapi.h>
#include <tchar.h>

#include <stdbool.h>

#define MONITOR_WMI_INSTANCE_PREFIX_LENGTH 128

typedef struct _monitor_t
{
	int index;

	MONITORINFOEX monitorInfo;										// Logical HMONITOR
	DISPLAY_DEVICE displayDevice;									// Standard display device information
	DISPLAY_DEVICE displayDeviceInterface;							// DeviceID is set using EDD_GET_DEVICE_INTERFACE_NAME
	PHYSICAL_MONITOR physicalMonitor;								// Physical monitor

	TCHAR wmiInstancePrefix[MONITOR_WMI_INSTANCE_PREFIX_LENGTH];	// Assumed prefix of the WMI instance path
	TCHAR wmiInstance[MONITOR_WMI_INSTANCE_PREFIX_LENGTH];			// Found WMI instance path
	bool hasWmiBrightness;											// 
	int wmiBrightness;
	int wmiMinBrightness;
	int wmiMaxBrightness;

	bool queriedOk;
	bool hasBrightness;
	int minBrightness;
	int maxBrightness;
	int brightness;

	struct _monitor_t *next;
} monitor_t;

void MonitorDump(FILE *file, monitor_t *monitor);
bool MonitorHasBrightness(monitor_t *monitor);
int MonitorGetBrightness(monitor_t *monitor);	// at time of last call to MonitorListRefreshBrightness()
void MonitorSetBrightness(monitor_t *monitor, int brightness);
const wchar_t *MonitorGetDescription(monitor_t *monitor);

monitor_t *MonitorListEnumerate(void);
void MonitorListRefreshBrightness(monitor_t *monitorList);
void MonitorListDestroy(monitor_t *monitorList);

#endif
