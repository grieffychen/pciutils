/*
 *      The PCI Library -- PCI config space access using Kernel Local Debugging Driver
 *
 *      Copyright (c) 2022 Pali Rohár <pali@kernel.org>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL v2+.
 *
 *	SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <windows.h>
#include <winioctl.h>

#include <stdio.h> /* for sprintf() */
#include <string.h> /* for memset() and memcpy() */

#include "internal.h"
#include "win32-helpers.h"

#ifndef ERROR_NOT_FOUND
#define ERROR_NOT_FOUND 1168
#endif

#ifndef LOAD_LIBRARY_AS_IMAGE_RESOURCE
#define LOAD_LIBRARY_AS_IMAGE_RESOURCE 0x20
#endif
#ifndef LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE
#define LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE 0x40
#endif

#ifndef IOCTL_KLDBG
#define IOCTL_KLDBG CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1, METHOD_NEITHER, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

#ifndef BUS_DATA_TYPE
#define BUS_DATA_TYPE LONG
#endif
#ifndef PCIConfiguration
#define PCIConfiguration (BUS_DATA_TYPE)4
#endif

#ifndef SYSDBG_COMMAND
#define SYSDBG_COMMAND ULONG
#endif
#ifndef SysDbgReadBusData
#define SysDbgReadBusData (SYSDBG_COMMAND)18
#endif
#ifndef SysDbgWriteBusData
#define SysDbgWriteBusData (SYSDBG_COMMAND)19
#endif

#ifndef _WIN64
typedef struct _SYSDBG_BUS_DATA64 {
  ULONG Address;
  u64 Buffer64; /* 32-bit process has to pass 64-bit wide pointer on 64-bit systems */
  ULONG Request;
  BUS_DATA_TYPE BusDataType;
  ULONG BusNumber;
  ULONG SlotNumber;
} SYSDBG_BUS_DATA64, *PSYSDBG_BUS_DATA64;
#endif
#ifndef SYSDBG_BUS_DATA
typedef struct _SYSDBG_BUS_DATA {
  ULONG Address;
  PVOID Buffer;
  ULONG Request;
  BUS_DATA_TYPE BusDataType;
  ULONG BusNumber;
  ULONG SlotNumber;
} SYSDBG_BUS_DATA, *PSYSDBG_BUS_DATA;
#define SYSDBG_BUS_DATA SYSDBG_BUS_DATA
#endif

#ifndef PCI_SEGMENT_BUS_NUMBER
typedef struct _PCI_SEGMENT_BUS_NUMBER {
  union {
    struct {
      ULONG BusNumber:8;
      ULONG SegmentNumber:16;
      ULONG Reserved:8;
    } bits;
    ULONG AsULONG;
  } u;
} PCI_SEGMENT_BUS_NUMBER, *PPCI_SEGMENT_BUS_NUMBER;
#define PCI_SEGMENT_BUS_NUMBER PCI_SEGMENT_BUS_NUMBER
#endif

#ifndef PCI_SLOT_NUMBER
typedef struct _PCI_SLOT_NUMBER {
  union {
    struct {
      ULONG DeviceNumber:5;
      ULONG FunctionNumber:3;
      ULONG Reserved:24;
    } bits;
    ULONG AsULONG;
  } u;
} PCI_SLOT_NUMBER, *PPCI_SLOT_NUMBER;
#define PCI_SLOT_NUMBER PCI_SLOT_NUMBER
#endif

#ifndef _WIN64
typedef struct _KLDBG64 {
  SYSDBG_COMMAND Command;
  u64 Buffer64; /* 32-bit process has to pass 64-bit wide pointer on 64-bit systems */
  DWORD BufferLength;
} KLDBG64, *PKLDBG64;
#endif
#ifndef KLDBG
typedef struct _KLDBG {
  SYSDBG_COMMAND Command;
  PVOID Buffer;
  DWORD BufferLength;
} KLDBG, *PKLDBG;
#define KLDBG KLDBG
#endif

static BOOL debug_privilege_enabled;
static LUID luid_debug_privilege;
static BOOL revert_only_privilege;
static HANDLE revert_token;

static HANDLE kldbg_dev = INVALID_HANDLE_VALUE;

static BOOL
win32_kldbg_pci_bus_data(BOOL WriteBusData, USHORT SegmentNumber, BYTE BusNumber, BYTE DeviceNumber, BYTE FunctionNumber, USHORT Address, PVOID Buffer, ULONG BufferSize, LPDWORD Length);

static BOOL
win32_check_driver(BYTE *driver_data, USHORT native_machine, USHORT *driver_machine_ptr)
{
  IMAGE_DOS_HEADER *dos_header;
  IMAGE_NT_HEADERS *nt_headers;

  if (native_machine == IMAGE_FILE_MACHINE_UNKNOWN)
    return FALSE;

  dos_header = (IMAGE_DOS_HEADER *)driver_data;
  if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
    return FALSE;

  nt_headers = (IMAGE_NT_HEADERS *)((BYTE *)dos_header + dos_header->e_lfanew);
  if (nt_headers->Signature != IMAGE_NT_SIGNATURE)
    return FALSE;

  if (!(nt_headers->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE))
    return FALSE;

  if (nt_headers->OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_NATIVE)
    return FALSE;

  *driver_machine_ptr = nt_headers->FileHeader.Machine;
  if (*driver_machine_ptr != native_machine)
    return FALSE;

  return TRUE;
}

static BOOL
win32_kldbg_unpack_driver(struct pci_access *a, LPTSTR driver_path, USHORT native_machine)
{
  BOOL use_kd_exe = FALSE;
  HMODULE exe_with_driver = NULL;
  HRSRC driver_resource_info = NULL;
  HGLOBAL driver_resource = NULL;
  BYTE *driver_data = NULL;
  DWORD driver_size = 0;
  HANDLE driver_handle = INVALID_HANDLE_VALUE;
  USHORT driver_machine = IMAGE_FILE_MACHINE_UNKNOWN;
  DWORD written = 0;
  DWORD error = 0;
  BOOL ret = FALSE;

  /* Try to find and open windbg.exe or kd.exe file in PATH. */
  exe_with_driver = LoadLibraryEx(TEXT("windbg.exe"), NULL, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!exe_with_driver)
    {
      use_kd_exe = TRUE;
      exe_with_driver = LoadLibraryEx(TEXT("kd.exe"), NULL, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    }
  if (!exe_with_driver)
    {
      error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND ||
          error == ERROR_MOD_NOT_FOUND)
        a->debug("Cannot find windbg.exe or kd.exe file in PATH.");
      else
        a->debug("Cannot load %s file: %s.", use_kd_exe ? "kd.exe" : "windbg.exe", win32_strerror(error));
      goto out;
    }

  /* kldbgdrv.sys is embedded in windbg.exe/kd.exe as a resource with name id 0x7777 and type id 0x4444. */
  driver_resource_info = FindResource(exe_with_driver, MAKEINTRESOURCE(0x7777), MAKEINTRESOURCE(0x4444));
  if (!driver_resource_info)
    {
      a->debug("Cannot find kldbgdrv.sys resource in %s file: %s.", use_kd_exe ? "kd.exe" : "windbg.exe", win32_strerror(GetLastError()));
      goto out;
    }

  driver_resource = LoadResource(exe_with_driver, driver_resource_info);
  if (!driver_resource)
    {
      a->debug("Cannot load kldbgdrv.sys resource from %s file: %s.", use_kd_exe ? "kd.exe" : "windbg.exe", win32_strerror(GetLastError()));
      goto out;
    }

  driver_size = SizeofResource(exe_with_driver, driver_resource_info);
  if (!driver_size)
    {
      a->debug("Cannot determinate size of kldbgdrv.sys resource from %s file: %s.", use_kd_exe ? "kd.exe" : "windbg.exe", win32_strerror(GetLastError()));
      goto out;
    }

  driver_data = LockResource(driver_resource);
  if (!driver_data)
    {
      a->debug("Cannot load kldbgdrv.sys resouce data from %s file: %s.", use_kd_exe ? "kd.exe" : "windbg.exe", win32_strerror(GetLastError()));
      goto out;
    }

  if (!win32_check_driver(driver_data, native_machine, &driver_machine))
    {
      if (native_machine == IMAGE_FILE_MACHINE_UNKNOWN)
        a->debug("Cannot use kldbgdrv.sys driver from %s file: Cannot determinate native machine architecture.", use_kd_exe ? "kd.exe" : "windbg.exe");
      else if (driver_machine == IMAGE_FILE_MACHINE_UNKNOWN)
        a->debug("Cannot use kldbgdrv.sys driver from %s file: Attached resource is not valid kernel driver.", use_kd_exe ? "kd.exe" : "windbg.exe");
      else
        a->debug("Cannot use kldbgdrv.sys driver from %s file: Driver machine architecture 0x%04x differs from native machine architecture 0x%04x.", use_kd_exe ? "kd.exe" : "windbg.exe", (unsigned)driver_machine, (unsigned)native_machine);
      goto out;
    }

  driver_handle = CreateFile(driver_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  if (driver_handle == INVALID_HANDLE_VALUE)
    {
      error = GetLastError();
      if (error != ERROR_FILE_EXISTS)
        {
          a->debug("Cannot create kldbgdrv.sys driver file in system32 directory: %s.", win32_strerror(error));
          goto out;
        }
      /* If driver file in system32 directory already exists then treat it as successfull unpack. */
      ret = TRUE;
      goto out;
    }

  if (!WriteFile(driver_handle, driver_data, driver_size, &written, NULL) ||
      written != driver_size)
    {
      a->debug("Cannot store kldbgdrv.sys driver file to system32 directory: %s.", win32_strerror(GetLastError()));
      /* On error, delete file from system32 directory to allow another unpack attempt. */
      CloseHandle(driver_handle);
      driver_handle = INVALID_HANDLE_VALUE;
      DeleteFile(driver_path);
      goto out;
    }

  a->debug("Driver kldbgdrv.sys was successfully unpacked from %s and stored in system32 directory...", use_kd_exe ? "kd.exe" : "windbg.exe");
  ret = TRUE;

out:
  if (driver_handle != INVALID_HANDLE_VALUE)
    CloseHandle(driver_handle);

  if (driver_resource)
    FreeResource(driver_resource);

  if (exe_with_driver)
    FreeLibrary(exe_with_driver);

  return ret;
}

static int
win32_kldbg_register_driver(struct pci_access *a, SC_HANDLE manager, SC_HANDLE *service)
{
  UINT system32_len;
  LPTSTR driver_path;
  HANDLE driver_handle;
  BOOL has_driver;
  HMODULE kernel32;
  USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
  BOOL fs_revert_needed = FALSE;
  PVOID fs_revert_value = NULL;
  BOOL (WINAPI *MyWow64DisableWow64FsRedirection)(PVOID *) = NULL;
  BOOL (WINAPI *MyWow64RevertWow64FsRedirection)(PVOID) = NULL;

  /*
   * COM library dbgeng.dll unpacks kldbg driver to file "\\system32\\kldbgdrv.sys"
   * and register this driver with service name kldbgdrv. Implement same behavior.
   * GetSystemDirectory() returns path to "\\system32" directory on all Windows versions.
   */

  system32_len = GetSystemDirectory(NULL, 0); /* Returns number of TCHARs plus 1 for nul-term. */
  if (!system32_len)
    system32_len = sizeof("C:\\Windows\\System32");

  driver_path = pci_malloc(a, (system32_len + sizeof("\\kldbgdrv.sys")-1) * sizeof(TCHAR));

  system32_len = GetSystemDirectory(driver_path, system32_len); /* Now it returns number of TCHARs without nul-term. */
  if (!system32_len)
    {
      system32_len = sizeof("C:\\Windows\\System32")-1;
      memcpy(driver_path, TEXT("C:\\Windows\\System32"), system32_len);
    }

  /* GetSystemDirectory returns path without backslash unless the system directory is the root directory. */
  if (driver_path[system32_len-1] != '\\')
    driver_path[system32_len++] = '\\';

  memcpy(driver_path + system32_len, TEXT("kldbgdrv.sys"), sizeof(TEXT("kldbgdrv.sys")));

  /*
   * For non-native processes to access System32 directory, it is required to
   * disable FsRedirection. FsRedirection is by default enabled and automatically
   * redirects all access to System32 directory from non-native processes to
   * different directory (e.g. to SysWOW64 directory).
   *
   * FsRedirection can be temporary disabled for he current thread by
   * Wow64DisableWow64FsRedirection() function and then reverted to the
   * previous state by Wow64RevertWow64FsRedirection() function. These
   * functions properly handle repeated calls.
   */

  if (win32_is_not_native_process(&native_machine))
    {
      if (native_machine == IMAGE_FILE_MACHINE_UNKNOWN)
        {
          a->debug("Cannot register new driver: Unable to detect native machine architecture from non-native process.");
          pci_mfree(driver_path);
          return 0;
        }
      kernel32 = GetModuleHandle(TEXT("kernel32.dll"));
      if (kernel32)
        {
          MyWow64DisableWow64FsRedirection = (void *)GetProcAddress(kernel32, "Wow64DisableWow64FsRedirection");
          MyWow64RevertWow64FsRedirection = (void *)GetProcAddress(kernel32, "Wow64RevertWow64FsRedirection");
        }
      if (!MyWow64DisableWow64FsRedirection || !MyWow64RevertWow64FsRedirection)
        {
          a->debug("Cannot register new driver: Unable to locate FsRedirection functions in kernel32.dll library.");
          pci_mfree(driver_path);
          return 0;
        }
      if (!MyWow64DisableWow64FsRedirection(&fs_revert_value))
        {
          a->debug("Cannot register new driver: Disabling FsRedirection for the current thread failed: %s.", win32_strerror(GetLastError()));
          pci_mfree(driver_path);
          return 0;
        }
      fs_revert_needed = TRUE;
    }
  else
    {
      native_machine = win32_get_process_machine();
    }

  has_driver = FALSE;
  driver_handle = CreateFile(driver_path, 0, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (driver_handle != INVALID_HANDLE_VALUE)
    CloseHandle(driver_handle);

  if (driver_handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND)
    {
      a->debug("Driver kldbgdrv.sys is missing, trying to unpack it from windbg.exe or kd.exe...");
      has_driver = win32_kldbg_unpack_driver(a, driver_path, native_machine);
    }
  else
    {
      /*
       * driver_path was either successfully opened or it cannot be opened for
       * any other reason than ERROR_FILE_NOT_FOUND. So expects that it exists.
       */
      has_driver = TRUE;
    }

  if (fs_revert_needed)
    {
      if (!MyWow64RevertWow64FsRedirection(fs_revert_value))
        a->warning("Reverting of FsRedirection for the current thread failed: %s.", win32_strerror(GetLastError()));
    }

  if (!has_driver)
    {
      pci_mfree(driver_path);
      return 0;
    }

  *service = CreateService(manager, TEXT("kldbgdrv"), TEXT("kldbgdrv"), SERVICE_START, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, driver_path, NULL, NULL, NULL, NULL, NULL);
  if (!*service)
    {
      if (GetLastError() != ERROR_SERVICE_EXISTS)
        {
          a->debug("Cannot create kldbgdrv service: %s.", win32_strerror(GetLastError()));
          pci_mfree(driver_path);
          return 0;
        }

      *service = OpenService(manager, TEXT("kldbgdrv"), SERVICE_START);
      if (!*service)
        {
          a->debug("Cannot open kldbgdrv service: %s.", win32_strerror(GetLastError()));
          pci_mfree(driver_path);
          return 0;
        }
    }

  a->debug("Service kldbgdrv was successfully registered...");
  pci_mfree(driver_path);
  return 1;
}

static int
win32_kldbg_start_driver(struct pci_access *a)
{
  SC_HANDLE manager = NULL;
  SC_HANDLE service = NULL;
  DWORD error = 0;
  int ret = 0;

  manager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
  if (!manager)
    manager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
  if (!manager)
    {
      a->debug("Cannot open Service Manager: %s.", win32_strerror(GetLastError()));
      return 0;
    }

  service = OpenService(manager, TEXT("kldbgdrv"), SERVICE_START);
  if (!service)
    {
      error = GetLastError();
      if (error != ERROR_SERVICE_DOES_NOT_EXIST)
        {
          a->debug("Cannot open kldbgdrv service: %s.", win32_strerror(error));
          goto out;
        }

      a->debug("Kernel Local Debugging Driver (kldbgdrv.sys) is not registered, trying to register it...");

      if (!win32_kldbg_register_driver(a, manager, &service))
        goto out;
    }

  if (!StartService(service, 0, NULL))
    {
      error = GetLastError();
      if (error != ERROR_SERVICE_ALREADY_RUNNING)
        {
          a->debug("Cannot start kldbgdrv service: %s.", win32_strerror(error));
          goto out;
        }
    }

  a->debug("Service kldbgdrv successfully started...");
  ret = 1;

out:
  if (service)
    CloseServiceHandle(service);

  if (manager)
    CloseServiceHandle(manager);

  return ret;
}

static int
win32_kldbg_setup(struct pci_access *a)
{
  OSVERSIONINFO version;
  DWORD ret_len;
  DWORD error;
  DWORD id;

  if (kldbg_dev != INVALID_HANDLE_VALUE)
    return 1;

  /* Check for Windows Vista (NT 6.0). */
  version.dwOSVersionInfoSize = sizeof(version);
  if (!GetVersionEx(&version) ||
      version.dwPlatformId != VER_PLATFORM_WIN32_NT ||
      version.dwMajorVersion < 6)
    {
      a->debug("Accessing PCI config space via Kernel Local Debugging Driver requires Windows Vista or higher version.");
      return 0;
    }

  kldbg_dev = CreateFile(TEXT("\\\\.\\kldbgdrv"), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (kldbg_dev == INVALID_HANDLE_VALUE)
    {
      error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND)
        {
          a->debug("Cannot open \"\\\\.\\kldbgdrv\" device: %s.", win32_strerror(error));
          return 0;
        }

      a->debug("Kernel Local Debugging Driver (kldbgdrv.sys) is not running, trying to start it...");

      if (!win32_kldbg_start_driver(a))
        return 0;

      kldbg_dev = CreateFile(TEXT("\\\\.\\kldbgdrv"), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      if (kldbg_dev == INVALID_HANDLE_VALUE)
        {
          error = GetLastError();
          a->debug("Cannot open \"\\\\.\\kldbgdrv\" device: %s.", win32_strerror(error));
          return 0;
        }
    }

  /*
   * Try to read PCI id register from PCI device 0000:00:00.0.
   * If this device does not exist and kldbg API is working then
   * kldbg returns success with read value 0xffffffff.
   */
  if (win32_kldbg_pci_bus_data(FALSE, 0, 0, 0, 0, 0, &id, sizeof(id), &ret_len) && ret_len == sizeof(id))
    return 1;

  error = GetLastError();

  a->debug("Cannot read PCI config space via Kernel Local Debugging Driver: %s.", win32_strerror(error));

  if (error != ERROR_ACCESS_DENIED)
    {
      CloseHandle(kldbg_dev);
      kldbg_dev = INVALID_HANDLE_VALUE;
      return 0;
    }

  a->debug("..Trying again with Debug privilege...");

  if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid_debug_privilege))
    {
      a->debug("Debug privilege is not supported.");
      CloseHandle(kldbg_dev);
      kldbg_dev = INVALID_HANDLE_VALUE;
      return 0;
    }

  if (!win32_enable_privilege(luid_debug_privilege, &revert_token, &revert_only_privilege))
    {
      a->debug("Process does not have right to enable Debug privilege.");
      CloseHandle(kldbg_dev);
      kldbg_dev = INVALID_HANDLE_VALUE;
      return 0;
    }

  if (win32_kldbg_pci_bus_data(FALSE, 0, 0, 0, 0, 0, &id, sizeof(id), &ret_len) && ret_len == sizeof(id))
    {
      a->debug("Succeeded.");
      debug_privilege_enabled = TRUE;
      return 1;
    }

  error = GetLastError();

  a->debug("Cannot read PCI config space via Kernel Local Debugging Driver: %s.", win32_strerror(error));

  CloseHandle(kldbg_dev);
  kldbg_dev = INVALID_HANDLE_VALUE;

  win32_revert_privilege(luid_debug_privilege, revert_token, revert_only_privilege);
  revert_token = NULL;
  revert_only_privilege = FALSE;
  return 0;
}

static int
win32_kldbg_detect(struct pci_access *a)
{
  if (!win32_kldbg_setup(a))
    return 0;

  return 1;
}

static void
win32_kldbg_init(struct pci_access *a)
{
  if (!win32_kldbg_setup(a))
    {
      a->debug("\n");
      a->error("PCI config space via Kernel Local Debugging Driver cannot be accessed.");
    }
}

static void
win32_kldbg_cleanup(struct pci_access *a UNUSED)
{
  if (kldbg_dev == INVALID_HANDLE_VALUE)
    return;

  CloseHandle(kldbg_dev);
  kldbg_dev = INVALID_HANDLE_VALUE;

  if (debug_privilege_enabled)
    {
      win32_revert_privilege(luid_debug_privilege, revert_token, revert_only_privilege);
      revert_token = NULL;
      revert_only_privilege = FALSE;
      debug_privilege_enabled = FALSE;
    }
}

struct acpi_mcfg {
  char signature[4];
  u32 length;
  u8 revision;
  u8 checksum;
  char oem_id[6];
  char oem_table_id[8];
  u32 oem_revision;
  char asl_compiler_id[4];
  u32 asl_compiler_revision;
  u64 reserved;
  struct {
    u64 address;
    u16 pci_segment;
    u8 start_bus_number;
    u8 end_bus_number;
    u32 reserved;
  } allocations[0];
} PCI_PACKED;

static void
win32_kldbg_scan(struct pci_access *a)
{
  /*
   * There is no kldbg API to retrieve list of PCI segments. WinDBG pci plugin
   * kext.dll loads debug symbols from pci.pdb file for kernel module pci.sys.
   * Then it reads kernel memory which belongs to PciSegmentList local variable
   * which is the first entry of struct _PCI_SEGMENT linked list. And then it
   * iterates all entries in linked list and reads SegmentNumber for each entry.
   *
   * This is extremly ugly hack and does not work on systems without installed
   * kernel debug symbol files.
   *
   * Do something less ugly. Retrieve ACPI MCFG table via GetSystemFirmwareTable
   * and parse all PCI segment numbers from it. ACPI MCFG table contains PCIe
   * ECAM definitions, so all PCI segment numbers.
   */

  UINT (WINAPI *MyGetSystemFirmwareTable)(DWORD FirmwareTableProviderSignature, DWORD FirmwareTableID, PVOID pFirmwareTableBuffer, DWORD BufferSize);
  int i, allocations_count;
  struct acpi_mcfg *mcfg;
  HMODULE kernel32;
  byte *segments;
  DWORD error;
  DWORD size;

  /* Always scan PCI segment 0. */
  pci_generic_scan_domain(a, 0);

  kernel32 = GetModuleHandle(TEXT("kernel32.dll"));
  if (!kernel32)
    return;

  /* Function GetSystemFirmwareTable() is available since Windows Vista. */
  MyGetSystemFirmwareTable = (void *)GetProcAddress(kernel32, "GetSystemFirmwareTable");
  if (!MyGetSystemFirmwareTable)
    return;

  /* 0x41435049 = 'ACPI', 0x4746434D = 'MCFG' */
  size = MyGetSystemFirmwareTable(0x41435049, 0x4746434D, NULL, 0);
  if (size == 0)
    {
      error = GetLastError();
      if (error == ERROR_INVALID_FUNCTION) /* ACPI is not present, so only PCI segment 0 is available. */
        return;
      else if (error == ERROR_NOT_FOUND) /* MCFG table is not present, so only PCI segment 0 is available. */
        return;
      a->debug("Cannot retrieve ACPI MCFG table: %s.\n", win32_strerror(error));
      return;
    }

  mcfg = pci_malloc(a, size);

  if (MyGetSystemFirmwareTable(0x41435049, 0x4746434D, mcfg, size) != size)
    {
      error = GetLastError();
      a->debug("Cannot retrieve ACPI MCFG table: %s.\n", win32_strerror(error));
      pci_mfree(mcfg);
      return;
    }

  if (size < sizeof(*mcfg) || size < mcfg->length)
    {
      a->debug("ACPI MCFG table is broken.\n");
      pci_mfree(mcfg);
      return;
    }

  segments = pci_malloc(a, 0xFFFF/8);
  memset(segments, 0, 0xFFFF/8);

  /* Scan all MCFG allocations and set available PCI segments into bit field. */
  allocations_count = (mcfg->length - ((unsigned char *)&mcfg->allocations - (unsigned char *)mcfg)) / sizeof(mcfg->allocations[0]);
  for (i = 0; i < allocations_count; i++)
    segments[mcfg->allocations[i].pci_segment / 8] |= 1 << (mcfg->allocations[i].pci_segment % 8);

  /* Skip PCI segment 0 which was already scanned. */
  for (i = 1; i < 0xFFFF; i++)
    if (segments[i / 8] & (1 << (i % 8)))
      pci_generic_scan_domain(a, i);

  pci_mfree(segments);
  pci_mfree(mcfg);
}

static BOOL
win32_kldbg_pci_bus_data(BOOL WriteBusData, USHORT SegmentNumber, BYTE BusNumber, BYTE DeviceNumber, BYTE FunctionNumber, USHORT Address, PVOID Buffer, ULONG BufferSize, LPDWORD Length)
{
  union {
    KLDBG native;
#ifndef _WIN64
    KLDBG64 ext64;
#endif
  } kldbg_cmd;
  union {
    SYSDBG_BUS_DATA native;
#ifndef _WIN64
    SYSDBG_BUS_DATA64 ext64;
#endif
  } sysdbg_cmd;
  PCI_SLOT_NUMBER pci_slot;
  PCI_SEGMENT_BUS_NUMBER pci_seg_bus;
  DWORD kldbg_cmd_size;
  DWORD sysdbg_cmd_size;
#ifndef _WIN64
  BOOL is_32bit_on_64bit_system = win32_is_32bit_on_64bit_system();
#endif

  memset(&pci_slot, 0, sizeof(pci_slot));
  memset(&kldbg_cmd, 0, sizeof(kldbg_cmd));
  memset(&sysdbg_cmd, 0, sizeof(sysdbg_cmd));
  memset(&pci_seg_bus, 0, sizeof(pci_seg_bus));

  pci_seg_bus.u.bits.BusNumber = BusNumber;
  pci_seg_bus.u.bits.SegmentNumber = SegmentNumber;

  pci_slot.u.bits.DeviceNumber = DeviceNumber;
  pci_slot.u.bits.FunctionNumber = FunctionNumber;

#ifndef _WIN64
  if (is_32bit_on_64bit_system)
    {
      sysdbg_cmd.ext64.Address = Address;
      sysdbg_cmd.ext64.Buffer64 = (u64)(ULONG)Buffer; /* extend 32-bit pointer to 64-bit */
      sysdbg_cmd.ext64.Request = BufferSize;
      sysdbg_cmd.ext64.BusDataType = PCIConfiguration;
      sysdbg_cmd.ext64.BusNumber = pci_seg_bus.u.AsULONG;
      sysdbg_cmd.ext64.SlotNumber = pci_slot.u.AsULONG;
      sysdbg_cmd_size = sizeof(sysdbg_cmd.ext64);
    }
  else
#endif
    {
      sysdbg_cmd.native.Address = Address;
      sysdbg_cmd.native.Buffer = Buffer;
      sysdbg_cmd.native.Request = BufferSize;
      sysdbg_cmd.native.BusDataType = PCIConfiguration;
      sysdbg_cmd.native.BusNumber = pci_seg_bus.u.AsULONG;
      sysdbg_cmd.native.SlotNumber = pci_slot.u.AsULONG;
      sysdbg_cmd_size = sizeof(sysdbg_cmd.native);
    }

#ifndef _WIN64
  if (is_32bit_on_64bit_system)
    {
      kldbg_cmd.ext64.Command = WriteBusData ? SysDbgWriteBusData : SysDbgReadBusData;
      kldbg_cmd.ext64.Buffer64 = (u64)(ULONG)&sysdbg_cmd; /* extend 32-bit pointer to 64-bit */
      kldbg_cmd.ext64.BufferLength = sysdbg_cmd_size;
      kldbg_cmd_size = sizeof(kldbg_cmd.ext64);
    }
  else
#endif
    {
      kldbg_cmd.native.Command = WriteBusData ? SysDbgWriteBusData : SysDbgReadBusData;
      kldbg_cmd.native.Buffer = &sysdbg_cmd;
      kldbg_cmd.native.BufferLength = sysdbg_cmd_size;
      kldbg_cmd_size = sizeof(kldbg_cmd.native);
    }

  *Length = 0;
  return DeviceIoControl(kldbg_dev, IOCTL_KLDBG, &kldbg_cmd, kldbg_cmd_size, &sysdbg_cmd, sysdbg_cmd_size, Length, NULL);
}

static int
win32_kldbg_read(struct pci_dev *d, int pos, byte *buf, int len)
{
  DWORD ret_len;

  if ((unsigned int)d->domain > 0xffff)
    return 0;

  if (!win32_kldbg_pci_bus_data(FALSE, d->domain, d->bus, d->dev, d->func, pos, buf, len, &ret_len))
    return 0;

  if (ret_len != (unsigned int)len)
    return 0;

  return 1;
}

static int
win32_kldbg_write(struct pci_dev *d, int pos, byte *buf, int len)
{
  DWORD ret_len;

  if ((unsigned int)d->domain > 0xffff)
    return 0;

  if (!win32_kldbg_pci_bus_data(TRUE, d->domain, d->bus, d->dev, d->func, pos, buf, len, &ret_len))
    return 0;

  if (ret_len != (unsigned int)len)
    return 0;

  return 1;
}

struct pci_methods pm_win32_kldbg = {
  .name = "win32-kldbg",
  .help = "Win32 PCI config space access using Kernel Local Debugging Driver",
  .detect = win32_kldbg_detect,
  .init = win32_kldbg_init,
  .cleanup = win32_kldbg_cleanup,
  .scan = win32_kldbg_scan,
  .fill_info = pci_generic_fill_info,
  .read = win32_kldbg_read,
  .write = win32_kldbg_write,
};
