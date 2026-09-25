// wdk_data.ixx — WDK metadata attribute handling for CppWinC.
// Provides helper functions and emitted-code templates for WDK driver API projection:
// DriverType, IoctlCode, KMDF/UMDF/WDF-specific helpers, IRP handling,
// buffer marshaling, and kernel-mode type support.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;

#include <string>
#include <string_view>
#include <cstdint>

export module wdk_data;

namespace cppwinc::wdk
{
    // DriverType strings
    export constexpr std::string_view driver_type_none = "WDM";
    export constexpr std::string_view driver_type_kmdf = "KMDF";
    export constexpr std::string_view driver_type_umdf = "UMDF";
    export constexpr std::string_view driver_type_wdf = "WDF";

    // Get driver type string from enum value
    export inline std::string_view get_driver_type(std::uint32_t dt)
    {
        switch (dt)
        {
        case 1: return driver_type_kmdf;
        case 2: return driver_type_umdf;
        case 3: return driver_type_wdf;
        default: return driver_type_wdf;
        }
    }

    // IOCTL code helpers
    export inline constexpr std::string_view ioctl_helpers_ixx = R"cppwinc(
    // IOCTL code helpers
    namespace wdk::impl
    {
        // IOCTL code extraction macros
        #define IOCTL_CTL_CODE(DeviceType, Function, Method, Access) \
            (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))

        #define IOCTL_DEVICE_TYPE(ioctl) (((ioctl) >> 16) & 0xFFFF)
        #define IOCTL_FUNCTION(ioctl) (((ioctl) >> 2) & 0xFFF)
        #define IOCTL_METHOD(ioctl) ((ioctl) & 0x3)
        #define IOCTL_ACCESS(ioctl) (((ioctl) >> 14) & 0x3)

        // METHOD_BUFFERED = 0, METHOD_IN_DIRECT = 1, METHOD_OUT_DIRECT = 2, METHOD_NEITHER = 3
        // FILE_ANY_ACCESS = 0, FILE_READ_ACCESS = 1, FILE_WRITE_ACCESS = 2, FILE_READ_WRITE_ACCESS = 3

        constexpr std::uint32_t METHOD_BUFFERED = 0;
        constexpr std::uint32_t METHOD_IN_DIRECT = 1;
        constexpr std::uint32_t METHOD_OUT_DIRECT = 2;
        constexpr std::uint32_t METHOD_NEITHER = 3;

        constexpr std::uint32_t FILE_ANY_ACCESS = 0;
        constexpr std::uint32_t FILE_READ_ACCESS = 1;
        constexpr std::uint32_t FILE_WRITE_ACCESS = 2;
        constexpr std::uint32_t FILE_READ_WRITE_ACCESS = 3;
    }
    )cppwinc";

    // KMDF helpers
    export inline constexpr std::string_view kmdf_helpers_ixx = R"cppwinc(
    // KMDF driver helpers
    namespace wdk::kmdf
    {
        // Driver entry point signature
        using DRIVER_INITIALIZE = NTSTATUS (*)(PDRIVER_OBJECT, PUNICODE_STRING);
        using DRIVER_UNLOAD = VOID (*)(PDRIVER_OBJECT);

        // KMDF device context helpers
        template<typename Context>
        Context* get_device_context(WDFDEVICE device)
        {
            return static_cast<Context*>(WdfDeviceWdmGetDeviceExtension(device));
        }

        // KMDF request context helpers
        template<typename Context>
        Context* get_request_context(WDFREQUEST request)
        {
            return static_cast<Context*>(WdfObjectGetTypedContext(request, WdfGetTypedContextPreallocationType(Context)));
        }
    }
    )cppwinc";

    // UMDF helpers
    export inline constexpr std::string_view umdf_helpers_ixx = R"cppwinc(
    // UMDF driver helpers
    namespace wdk::umdf
    {
        // UMDF driver entry point signature
        using PFN_WDF_DRIVER_DEVICE_ADD = NTSTATUS (*)(WDFDRIVER, PWDFDEVICE_INIT);

        // UMDF device context helpers
        template<typename Context>
        Context* get_device_context(WDFDEVICE device)
        {
            return static_cast<Context*>(WdfDeviceGetDeviceObject()->DeviceExtension);
        }
    }
    )cppwinc";

    // WDF common helpers
    export inline constexpr std::string_view wdf_helpers_ixx = R"cppwinc(
    // WDF common helpers
    namespace wdk::wdf
    {
        // WDF object type helpers
        template<typename T>
        T* get_object_context(WDFOBJECT object)
        {
            return static_cast<T*>(WdfObjectGetTypedContext(object, WdfGetTypedContextPreallocationType(T)));
        }

        // WDF queue context helpers
        template<typename Context>
        Context* get_queue_context(WDFQUEUE queue)
        {
            return static_cast<Context*>(WdfObjectGetTypedContext(queue, WdfGetTypedContextPreallocationType(Context)));
        }

        // WDF timer helpers
        inline BOOLEAN create_timer(WDFTIMER* timer, PFN_WDF_TIMER callback_routine, WDFTIMERTYPE type = WdfTimerStopOnSuspend)
        {
            WDF_TIMER_CONFIG config;
            WDF_TIMER_CONFIG_INIT(&config, callback_routine);
            return SUCCEEDED(WdfTimerCreate(&config, type, WDF_NO_OBJECT_ATTRIBUTES, timer));
        }
    }
    )cppwinc";

    // IRP helpers
    export inline constexpr std::string_view irp_helpers_ixx = R"cppwinc(
    // IRP handling helpers
    namespace wdk::irp
    {
        // IRP major function codes
        constexpr UCHAR IRP_MJ_CREATE = 0x00;
        constexpr UCHAR IRP_MJ_CREATE_NAMED_PIPE = 0x01;
        constexpr UCHAR IRP_MJ_CLOSE = 0x02;
        constexpr UCHAR IRP_MJ_READ = 0x03;
        constexpr UCHAR IRP_MJ_WRITE = 0x04;
        constexpr UCHAR IRP_MJ_QUERY_INFORMATION = 0x05;
        constexpr UCHAR IRP_MJ_SET_INFORMATION = 0x06;
        constexpr UCHAR IRP_MJ_QUERY_EA = 0x07;
        constexpr UCHAR IRP_MJ_SET_EA = 0x08;
        constexpr UCHAR IRP_MJ_FLUSH_BUFFERS = 0x09;
        constexpr UCHAR IRP_MJ_QUERY_VOLUME_INFORMATION = 0x0A;
        constexpr UCHAR IRP_MJ_SET_VOLUME_INFORMATION = 0x0B;
        constexpr UCHAR IRP_MJ_DIRECTORY_CONTROL = 0x0C;
        constexpr UCHAR IRP_MJ_FILE_CONTROL = 0x0D;
        constexpr UCHAR IRP_MJ_DEVICE_CONTROL = 0x0E;
        constexpr UCHAR IRP_MJ_INTERNAL_DEVICE_CONTROL = 0x0F;
        constexpr UCHAR IRP_MJ_SHUTDOWN = 0x10;
        constexpr UCHAR IRP_MJ_LOCK_CONTROL = 0x11;
        constexpr UCHAR IRP_MJ_CLEANUP = 0x12;
        constexpr UCHAR IRP_MJ_CREATE_MAILSLOT = 0x13;
        constexpr UCHAR IRP_MJ_QUERY_SECURITY = 0x14;
        constexpr UCHAR IRP_MJ_SET_SECURITY = 0x15;
        constexpr UCHAR IRP_MJ_POWER = 0x16;
        constexpr UCHAR IRP_MJ_SYSTEM_CONTROL = 0x17;
        constexpr UCHAR IRP_MJ_DEVICE_CHANGE = 0x18;
        constexpr UCHAR IRP_MJ_QUERY_QUOTA = 0x19;
        constexpr UCHAR IRP_MJ_SET_QUOTA = 0x1A;
        constexpr UCHAR IRP_MJ_PNP = 0x1B;
        constexpr UCHAR IRP_MJ_PNP_POWER = 0x1B;
        constexpr UCHAR IRP_MJ_MAXIMUM_FUNCTION = 0x1B;

        // NTSTATUS codes
        constexpr NTSTATUS STATUS_SUCCESS = 0x00000000;
        constexpr NTSTATUS STATUS_UNSUCCESSFUL = 0xC0000001;
        constexpr NTSTATUS STATUS_BUFFER_OVERFLOW = 0x80000005;
        constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL = 0xC0000023;
        constexpr NTSTATUS STATUS_INVALID_PARAMETER = 0xC000000D;
        constexpr NTSTATUS STATUS_NO_MEMORY = 0xC0000017;
        constexpr NTSTATUS STATUS_INSUFFICIENT_RESOURCES = 0xC000009A;

        // Buffer helpers
        inline NTSTATUS get_input_buffer(IRP* irp, void** buffer, SIZE_T* size)
        {
            *buffer = irp->AssociatedIrp.SystemBuffer;
            *size = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.InputBufferLength;
            return STATUS_SUCCESS;
        }

        inline NTSTATUS get_output_buffer(IRP* irp, void** buffer, SIZE_T* size)
        {
            *buffer = irp->AssociatedIrp.SystemBuffer;
            *size = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
            return STATUS_SUCCESS;
        }

        inline NTSTATUS complete_irp(IRP* irp, NTSTATUS status, ULONG_PTR information = 0)
        {
            irp->IoStatus.Status = status;
            irp->IoStatus.Information = information;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return status;
        }
    }
    )cppwinc";

    // Buffer marshaling helpers
    export inline constexpr std::string_view buffer_helpers_ixx = R"cppwinc(
    // WDF buffer marshaling helpers
    namespace wdk::buffer
    {
        // Get request input buffer
        inline NTSTATUS get_input_buffer(WDFREQUEST request, void** buffer, SIZE_T* size)
        {
            NTSTATUS status;
            WDFMEMORY memory;
            status = WdfRequestRetrieveInputMemory(request, &memory);
            if (NT_SUCCESS(status))
            {
                *buffer = WdfMemoryGetBuffer(memory, size);
            }
            return status;
        }

        // Get request output buffer
        inline NTSTATUS get_output_buffer(WDFREQUEST request, void** buffer, SIZE_T* size)
        {
            NTSTATUS status;
            WDFMEMORY memory;
            status = WdfRequestRetrieveOutputMemory(request, &memory);
            if (NT_SUCCESS(status))
            {
                *buffer = WdfMemoryGetBuffer(memory, size);
            }
            return status;
        }

        // Get request output length
        inline NTSTATUS get_output_length(WDFREQUEST request, SIZE_T* length)
        {
            NTSTATUS status;
            WDFMEMORY memory;
            status = WdfRequestRetrieveOutputMemory(request, &memory);
            if (NT_SUCCESS(status))
            {
                *length = WdfMemoryGetBufferSize(memory);
            }
            return status;
        }

        // Complete request with status
        inline void complete_request(WDFREQUEST request, NTSTATUS status, SIZE_T bytes_transferred = 0)
        {
            WdfRequestCompleteWithInformation(request, status, bytes_transferred);
        }
    }
    )cppwinc";
}
