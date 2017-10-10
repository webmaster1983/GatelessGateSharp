﻿// phymem_wrapper.cpp : Defines the exported functions for the DLL application.
//

#include "pch.h"
#include "phymem.h"
#include "phymem_wrapper.h"



BOOL InstallDriver(PCSTR pszDriverPath, PCSTR pszDriverName);
BOOL RemoveDriver(PCSTR pszDriverName);
BOOL StartDriver(PCSTR pszDriverName);
BOOL StopDriver(PCSTR pszDriverName);


BOOL InstallDriver(PCSTR pszDriverPath, PCSTR pszDriverName)
{
	SC_HANDLE hSCManager;
	SC_HANDLE hService;

	//Remove any previous instance of the driver
	RemoveDriver(pszDriverName);

	hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hSCManager)
	{
		//Install the driver
		hService = CreateServiceA(hSCManager,
			pszDriverName,
			pszDriverName,
			SERVICE_ALL_ACCESS,
			SERVICE_KERNEL_DRIVER,
			SERVICE_DEMAND_START,
			SERVICE_ERROR_NORMAL,
			pszDriverPath,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL);

		CloseServiceHandle(hSCManager);

		if (hService == NULL)
			return FALSE;
	}
	else
		return FALSE;

	CloseServiceHandle(hService);

	return TRUE;
}

BOOL RemoveDriver(PCSTR pszDriverName)
{
	SC_HANDLE hSCManager;
	SC_HANDLE hService;
	BOOL bResult;

	StopDriver(pszDriverName);

	hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hSCManager)
	{
		hService = OpenServiceA(hSCManager, pszDriverName, SERVICE_ALL_ACCESS);

		CloseServiceHandle(hSCManager);

		if (hService)
		{
			bResult = DeleteService(hService);

			CloseServiceHandle(hService);
		}
		else
			return FALSE;
	}
	else
		return FALSE;

	return bResult;
}

BOOL StartDriver(PCSTR pszDriverName)
{
	SC_HANDLE hSCManager;
	SC_HANDLE hService;
	BOOL bResult;

	hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hSCManager)
	{
		hService = OpenServiceA(hSCManager, pszDriverName, SERVICE_ALL_ACCESS);

		CloseServiceHandle(hSCManager);

		if (hService)
		{
			bResult = StartService(hService, 0, NULL);
			if (bResult == FALSE)
			{
				if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
					bResult = TRUE;
			}

			CloseServiceHandle(hService);
		}
		else
			return FALSE;
	}
	else
		return FALSE;

	return bResult;
}

BOOL StopDriver(PCSTR pszDriverName)
{
	SC_HANDLE hSCManager;
	SC_HANDLE hService;
	SERVICE_STATUS ServiceStatus;
	BOOL bResult;

	hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hSCManager)
	{
		hService = OpenServiceA(hSCManager, pszDriverName, SERVICE_ALL_ACCESS);

		CloseServiceHandle(hSCManager);

		if (hService)
		{
			bResult = ControlService(hService, SERVICE_CONTROL_STOP, &ServiceStatus);

			CloseServiceHandle(hService);
		}
		else
			return FALSE;
	}
	else
		return FALSE;

	return bResult;
}


#define RSDT_PTR_MAGIC_STRING "RSD PTR "
#define RSDT_PTR_MAGIC_STRING_RANGE_BASE 1024
#define RSDT_PTR_MAGIC_STRING_RANGE_SIZE (0x100000 - RSDT_PTR_MAGIC_STRING_RANGE_BASE)
#define RSDT_MAGIC_STRING "MCFG"
#define MAX_NUM_PCIE_BUSES 64 // FIXME
#define PCIE_MEMORY_MAP_SIZE (MAX_NUM_PCIE_BUSES * 1024 * 1024)


HANDLE hDriver = INVALID_HANDLE_VALUE;
struct ACPISDTHeader {
	char Signature[4];
	uint32_t Length;
	uint8_t Revision;
	uint8_t Checksum;
	char OEMID[6];
	char OEMTableID[8];
	uint32_t OEMRevision;
	uint32_t CreatorID;
	uint32_t CreatorRevision;
};
struct RSDT {
	struct ACPISDTHeader h;
	uint32_t PointerToOtherSDT[];
};


BOOL GetFirmwarePrivilege()
{
	BOOL result = FALSE;
	HANDLE processToken = NULL;
	TOKEN_PRIVILEGES privileges = { 0 };
	HANDLE process = GetCurrentProcess();
	LUID luid = { 0 };

	if (OpenProcessToken(process, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &processToken))
	{
		if (LookupPrivilegeValue(NULL, SE_SYSTEM_ENVIRONMENT_NAME, &luid))
		{
			privileges.PrivilegeCount = 1;
			privileges.Privileges[0].Luid = luid;
			privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

			if (AdjustTokenPrivileges(processToken, FALSE, &privileges, sizeof(TOKEN_PRIVILEGES), NULL, 0))
			{
				result = TRUE;
			}
		}

		CloseHandle(processToken);
	}

	CloseHandle(process);

	return result;
}


static uint32_t get_pcie_mapped_memory_addr()
{
	uint32_t rsdt_table_base = 0;
	char *virtual_addr = (char*)MapPhyMem(RSDT_PTR_MAGIC_STRING_RANGE_BASE, RSDT_PTR_MAGIC_STRING_RANGE_SIZE);
	unsigned int rsdt_ptr_offset;
	for (rsdt_ptr_offset = 0; rsdt_ptr_offset < RSDT_PTR_MAGIC_STRING_RANGE_SIZE - strlen(RSDT_PTR_MAGIC_STRING); rsdt_ptr_offset += 16) {
		if (strncmp(virtual_addr + rsdt_ptr_offset, RSDT_PTR_MAGIC_STRING, strlen(RSDT_PTR_MAGIC_STRING)) == 0)
			break;
	}
	if (rsdt_ptr_offset < RSDT_PTR_MAGIC_STRING_RANGE_SIZE - strlen(RSDT_PTR_MAGIC_STRING)) {
		//printf("Pointer to ACPI Root System Description Table (RSDT) found at 0x%08x.\n", (unsigned int)virtual_addr + rsdt_ptr_offset);
		rsdt_table_base = *(uint32_t *)(virtual_addr + rsdt_ptr_offset + 0x10);
		UnmapPhyMem(virtual_addr, RSDT_PTR_MAGIC_STRING_RANGE_SIZE);

		RSDT *rsdt = (RSDT *)MapPhyMem(rsdt_table_base, sizeof(rsdt->h));
		int entries = (rsdt->h.Length - sizeof(rsdt->h)) / 4;
		uint32_t rsdt_table_size = rsdt->h.Length;
		UnmapPhyMem(rsdt, sizeof(rsdt->h));
		rsdt = (RSDT *)MapPhyMem(rsdt_table_base, rsdt_table_size);
		//printf("entries: %d\n", entries);

		ACPISDTHeader *h = NULL;
		for (int i = 0; i < entries; i++)
		{
			h = (ACPISDTHeader *)MapPhyMem(rsdt->PointerToOtherSDT[i], 0x30);
			//printf("signature: %c%c%c%c\n", h->Signature[0], h->Signature[1], h->Signature[2], h->Signature[3]);
			if (!strncmp(h->Signature, RSDT_MAGIC_STRING, 4))
				break;
			UnmapPhyMem(h, 0x30);
			h = NULL;
		}

		if (h) {
			//printf("ACPI Root System Description Table (RSDT) found at 0x%08x.\n", (unsigned int)h);
		}
		else {
			//printf("ACPI Root System Description Table (RSDT) not found.\n");
			UnmapPhyMem(rsdt, rsdt_table_size);
			UnmapPhyMem(h, 0x30);
			return 0;
		}
		uint32_t pcie_mapped_memory_addr = *(uint32_t *)((uint8_t *)h + 0x2c);
		//printf("PCIe Memory Map found at 0x%08x.\n", pcie_mapped_memory_addr);
		UnmapPhyMem(rsdt, rsdt_table_size);
		UnmapPhyMem(h, 0x30);

		return pcie_mapped_memory_addr;
	}
	else {
		UnmapPhyMem(virtual_addr, RSDT_PTR_MAGIC_STRING_RANGE_SIZE);
		uint8_t mcfg[1024];
		uint32_t mcfg_size;
		if (GetFirmwarePrivilege() && ((mcfg_size = GetSystemFirmwareTable('ACPI', 'GFCM', mcfg, sizeof(mcfg))) > 0)) {
			//printf("MCFG was retrieved through GetSystemFirmwareTable().\n");
			return *(uint32_t *)((uint8_t *)mcfg + 0x2c);
		}
		else {
			//printf("Pointer to ACPI Root System Description Table (RSDT) not found (%u)\n", GetLastError());
			return 0;
		}
	}

}

static bool has_elevated_privileges() {
	HANDLE token = nullptr;
	BOOL result = false;
	TOKEN_ELEVATION elevation;
	DWORD size = sizeof(TOKEN_ELEVATION);

	OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
	if (!token)
		return false;

	if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
		result = elevation.TokenIsElevated;
	CloseHandle(token);
	return result;
}

//get driver(phymem.sys) full path
static BOOL GetDriverPath(PSTR szDriverPath)
{
	PSTR pszSlash;

	GetModuleFileNameA(GetModuleHandle(NULL), szDriverPath, MAX_PATH);
	if (GetLastError())
		return FALSE;

	pszSlash = strrchr(szDriverPath, '\\');

	if (pszSlash)
		pszSlash[1] = '\0';
	else
		return FALSE;

	return TRUE;
}

static std::recursive_mutex phymem_mutex;

//install and start driver
BOOL LoadPhyMemDriver()
{
	phymem_mutex.lock();

	BOOL result = FALSE;
	CHAR szDriverPath[MAX_PATH];

	if (!has_elevated_privileges())
		goto end;

	hDriver = CreateFileA("\\\\.\\PhyMem",
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);

	//If the driver is not running, install it
	if (hDriver == INVALID_HANDLE_VALUE)
	{
		GetDriverPath(szDriverPath);
		strcat(szDriverPath, "phymem.sys");
		if (!InstallDriver(szDriverPath, "PHYMEM"))
			goto end;
		if (!StartDriver("PHYMEM"))
			goto end;

		hDriver = CreateFileA("\\\\.\\PhyMem",
			GENERIC_READ | GENERIC_WRITE,
			0,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL);

		if (hDriver == INVALID_HANDLE_VALUE)
			goto end;
	}

	result = TRUE;

end:
	phymem_mutex.unlock();
	return result;
}

//stop and remove driver
VOID UnloadPhyMemDriver()
{
	phymem_mutex.lock();
	if (hDriver != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hDriver);
		hDriver = INVALID_HANDLE_VALUE;
	}

	StopDriver("PHYMEM");
	RemoveDriver("PHYMEM");
	phymem_mutex.unlock();
}

//map physical memory to user space
PVOID MapPhyMem(DWORD64 phyAddr, DWORD memSize)
{
	phymem_mutex.lock();

	PVOID pVirAddr = NULL;	//mapped virtual addr
	PHYMEM_MEM pm;
	DWORD dwBytes = 0;
	BOOL bRet = FALSE;

	pm.pvAddr = (PVOID)phyAddr;	//physical address
	pm.dwSize = memSize;	//memory size

	if (hDriver != INVALID_HANDLE_VALUE)
	{
		bRet = DeviceIoControl(hDriver, IOCTL_PHYMEM_MAP, &pm,
			sizeof(PHYMEM_MEM), &pVirAddr, sizeof(PVOID), &dwBytes, NULL);
	}

	phymem_mutex.unlock();
	return (bRet && dwBytes == sizeof(PVOID)) ? pVirAddr : NULL;
}

//unmap memory
VOID UnmapPhyMem(PVOID pVirAddr, DWORD memSize)
{
	phymem_mutex.lock();
	PHYMEM_MEM pm;
	DWORD dwBytes = 0;

	pm.pvAddr = pVirAddr;	//virtual address
	pm.dwSize = memSize;	//memory size

	if (hDriver != INVALID_HANDLE_VALUE)
	{
		DeviceIoControl(hDriver, IOCTL_PHYMEM_UNMAP, &pm,
			sizeof(PHYMEM_MEM), NULL, 0, &dwBytes, NULL);
	}
	phymem_mutex.unlock();
}

//read 1 byte from port
BYTE ReadPortByte(WORD portAddr)
{
	phymem_mutex.lock();
	PHYMEM_PORT pp;
	DWORD pv = 0;	//returned port value
	DWORD dwBytes;

	pp.dwPort = portAddr;
	pp.dwSize = 1;	//1 byte

	if (hDriver != INVALID_HANDLE_VALUE)
	{
		DeviceIoControl(hDriver, IOCTL_PHYMEM_GETPORT, &pp,
			sizeof(PHYMEM_PORT), &pv, sizeof(DWORD), &dwBytes, NULL);
	}

	phymem_mutex.unlock();
	return (BYTE)pv;
}

//read 2 bytes from port
WORD ReadPortWord(WORD portAddr)
{
	phymem_mutex.lock();
	PHYMEM_PORT pp;
	DWORD pv = 0;	//returned port value
	DWORD dwBytes;

	pp.dwPort = portAddr;
	pp.dwSize = 2;	//2 bytes

	if (hDriver != INVALID_HANDLE_VALUE)
	{
		DeviceIoControl(hDriver, IOCTL_PHYMEM_GETPORT, &pp,
			sizeof(PHYMEM_PORT), &pv, sizeof(DWORD), &dwBytes, NULL);
	}

	phymem_mutex.unlock();
	return (WORD)pv;
}

//read 4 bytes from port
DWORD ReadPortLong(WORD portAddr)
{
	phymem_mutex.lock();
	PHYMEM_PORT pp;
	DWORD pv = 0;	//returned port value
	DWORD dwBytes;

	pp.dwPort = portAddr;
	pp.dwSize = 4;	//4 bytes

	if (hDriver != INVALID_HANDLE_VALUE)
	{
		DeviceIoControl(hDriver, IOCTL_PHYMEM_GETPORT, &pp,
			sizeof(PHYMEM_PORT), &pv, sizeof(DWORD), &dwBytes, NULL);
	}

	phymem_mutex.unlock();
	return pv;
}

//write 1 byte to port
VOID WritePortByte(WORD portAddr, BYTE portValue)
{
	phymem_mutex.lock();
	PHYMEM_PORT pp;
	DWORD dwBytes;

	pp.dwPort = portAddr;
	pp.dwValue = portValue;
	pp.dwSize = 1;	//1 byte

	if (hDriver != INVALID_HANDLE_VALUE)
	{
		DeviceIoControl(hDriver, IOCTL_PHYMEM_SETPORT, &pp,
			sizeof(PHYMEM_PORT), NULL, 0, &dwBytes, NULL);
	}
	phymem_mutex.unlock();
}

//write 2 bytes to port
VOID WritePortWord(WORD portAddr, WORD portValue)
{
	phymem_mutex.lock();
	PHYMEM_PORT pp;
	DWORD dwBytes;

	pp.dwPort = portAddr;
	pp.dwValue = portValue;
	pp.dwSize = 2;	//2 bytes

	if (hDriver != INVALID_HANDLE_VALUE)
	{
		DeviceIoControl(hDriver, IOCTL_PHYMEM_SETPORT, &pp,
			sizeof(PHYMEM_PORT), NULL, 0, &dwBytes, NULL);
	}
	phymem_mutex.unlock();
}

//write 4 bytes to port
VOID WritePortLong(WORD portAddr, DWORD portValue)
{
	phymem_mutex.lock();
	PHYMEM_PORT pp;
	DWORD dwBytes;

	pp.dwPort = portAddr;
	pp.dwValue = portValue;
	pp.dwSize = 4;	//4 bytes

	if (hDriver != INVALID_HANDLE_VALUE)
	{
		DeviceIoControl(hDriver, IOCTL_PHYMEM_SETPORT, &pp,
			sizeof(PHYMEM_PORT), NULL, 0, &dwBytes, NULL);
	}
	phymem_mutex.unlock();
}

//read pci configuration
BOOL ReadPCI(DWORD busNum, DWORD devNum, DWORD funcNum,
	DWORD regOff, DWORD bytes, PVOID pValue)
{
	phymem_mutex.lock();
	uint32_t pcie_mapped_memory_addr = get_pcie_mapped_memory_addr();
	if (!pcie_mapped_memory_addr) {
		//printf("get_pcie_mapped_memory_addr() failed.\n");
		phymem_mutex.unlock();
		return FALSE;
	}

	char *virtual_addr = (char*)MapPhyMem(pcie_mapped_memory_addr + 4096 * (funcNum + 8 * (devNum + 32 * busNum)), 4096);
	if (!virtual_addr) {
		//printf("MapPhyMem() failed.\n");
		phymem_mutex.unlock();
		return FALSE;
	}

	memcpy(pValue, virtual_addr + regOff, bytes);
	UnmapPhyMem(virtual_addr, 4096);

	phymem_mutex.unlock();
	return TRUE;
}

//write pci configuration
BOOL WritePCI(DWORD busNum, DWORD devNum, DWORD funcNum,
	DWORD regOff, DWORD bytes, PVOID pValue)
{
	phymem_mutex.lock();
	uint32_t pcie_mapped_memory_addr = get_pcie_mapped_memory_addr();
	if (!pcie_mapped_memory_addr) {
		phymem_mutex.unlock();
		return FALSE;
	}

	char *virtual_addr = (char*)MapPhyMem(pcie_mapped_memory_addr + 4096 * (funcNum + 8 * (devNum + 32 * busNum)), 4096);
	if (!virtual_addr){
		phymem_mutex.unlock();
		return FALSE;
	}

	memcpy(virtual_addr + regOff, pValue, bytes);
	UnmapPhyMem(virtual_addr, 4096);

	phymem_mutex.unlock();
	return TRUE;
}
