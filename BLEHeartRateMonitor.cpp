#include "pch.h"
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>
#include <atomic>
#include <mutex>
#include <winrt/Windows.Foundation.Collections.h>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Storage::Streams;

// Global variables for thread-safe data sharing
std::atomic<int> g_heartRate(0);
std::atomic<bool> g_shouldStop(false);
std::mutex g_bleMutex;
HANDLE g_workerThread = nullptr;

// BLE Components (protected by mutex)
winrt::guid g_hrServiceUuid{ 0x0000180D, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB} };
winrt::guid g_hrMeasurementUuid{ 0x00002A37, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB} };
BluetoothLEDevice g_bleDevice = nullptr;
GattCharacteristic g_hrCharacteristic = nullptr;

DWORD WINAPI BleWorkerThread(LPVOID lpParam)
{
    // **Use multi-threaded apartment** instead of single-threaded
    init_apartment(apartment_type::multi_threaded);

    try {
        // --- Perform the same BLE device/characteristic discovery ---
        auto selector = GattDeviceService::GetDeviceSelectorFromUuid(g_hrServiceUuid);
        auto devices = DeviceInformation::FindAllAsync(selector).get();
        if (devices.Size() == 0) {
            return 27;
        }

        auto deviceInfo = devices.GetAt(0);

        {
            std::lock_guard<std::mutex> lock(g_bleMutex);
            g_bleDevice = BluetoothLEDevice::FromIdAsync(deviceInfo.Id()).get();
        }

        if (!g_bleDevice) {
            return 2;
        }

        auto serviceResult = g_bleDevice.GetGattServicesForUuidAsync(g_hrServiceUuid).get();
        if (serviceResult.Status() != GattCommunicationStatus::Success || serviceResult.Services().Size() == 0) {
            return 3;
        }
        auto hrService = serviceResult.Services().GetAt(0);

        auto charResult = hrService.GetCharacteristicsForUuidAsync(g_hrMeasurementUuid).get();
        if (charResult.Status() != GattCommunicationStatus::Success || charResult.Characteristics().Size() == 0) {
            return 4;
        }

        {
            std::lock_guard<std::mutex> lock(g_bleMutex);
            g_hrCharacteristic = charResult.Characteristics().GetAt(0);
        }

        auto status = g_hrCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
        if (status != GattCommunicationStatus::Success) {
            return 5;
        }

        // Subscribe to notifications
        g_hrCharacteristic.ValueChanged(
            [](auto&&, GattValueChangedEventArgs const& args)
            {
                DataReader reader = DataReader::FromBuffer(args.CharacteristicValue());
                uint8_t flags = reader.ReadByte();
                uint16_t rate = (flags & 0x01) ? reader.ReadUInt16() : reader.ReadByte();
                g_heartRate = rate;
            }
        );

        // keep thread alive idk, källa: deepseak
        while (!g_shouldStop)
        {
            ::Sleep(100);
        }

        /*
        // Cleanup
        if (g_hrCharacteristic) {
            g_hrCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::None).get();
        }

        if (g_bleDevice)
        {
            g_bleDevice.Close();
        }
        uninit_apartment();

        {
            std::lock_guard<std::mutex> lock(g_bleMutex);
            g_hrCharacteristic = nullptr;
            g_bleDevice = nullptr;
        }
        */
    }
    catch (...)
    {
        //uninit_apartment();
        return 6;
    }

    //uninit_apartment();
    return 0;
}


extern "C" {
    __declspec(dllexport) void StartHrMonitoring() {
        if (g_workerThread) return;
        g_shouldStop = false;
        g_workerThread = CreateThread(NULL, 0, BleWorkerThread, NULL, 0, NULL);
    }

    __declspec(dllexport) int StopHrMonitoring() {
        
        if (!g_workerThread) return 45;
        g_shouldStop = true;
        
        // Force cleanup regardless of thread state
        /*
        {
            
            std::lock_guard<std::mutex> lock(g_bleMutex);
            if (g_hrCharacteristic) {
                g_hrCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue::None).get();
                g_hrCharacteristic = nullptr;
            }
            if (g_bleDevice) {
                g_bleDevice.Close();
                g_bleDevice = nullptr;
            }
        }

        WaitForSingleObject(g_workerThread, 5000); // Wait up to 5 seconds
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
        */
        return 87;
    }

    __declspec(dllexport) int GetLatestHeartRate() {
        return g_heartRate.load();
    }
}
