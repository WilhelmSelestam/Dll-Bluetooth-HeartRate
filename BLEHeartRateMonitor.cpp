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


// --- Callbacks ---
typedef void(__stdcall* StatusCallback)(int status, const char* message);
typedef void(__stdcall* HeartRateCallback)(int bpm);

StatusCallback g_statusCallback = nullptr;
HeartRateCallback g_hrCallback = nullptr;

// --- Threading & State ---
std::atomic<bool> g_shouldStop(false);
std::thread g_workerThread; // Use std::thread for easier management
std::mutex g_callbackMutex; // Protect callback pointers
std::atomic<int> g_currentState(0); // Define states: 0=Idle, 1=Connecting, 2=Connected, 3=Error etc.

// --- BLE Components (managed only by worker thread) ---
// Keep UUIDs global or pass them around
winrt::guid g_hrServiceUuid{ 0x0000180D, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB} };
winrt::guid g_hrMeasurementUuid{ 0x00002A37, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB} };

// Forward declaration
void ReportStatus(int status, const char* message);
void ReportHeartRate(int bpm);

// --- Worker Thread Function ---
void BleWorkerLogic() {
    // Use RAII for apartment initialization/uninitialization
    winrt::apartment_context ui_thread; // Capture calling context if needed for marshaling back (optional)
    init_apartment(apartment_type::multi_threaded); // Or single_threaded if needed

    BluetoothLEDevice bleDevice = nullptr;
    GattCharacteristic hrCharacteristic = nullptr;
    winrt::event_token valueChangedToken{};
    winrt::event_token connectionStatusToken{};

    try {
        ReportStatus(1, "Starting Scan..."); // Status: Scanning
        // --- Device Discovery (using DeviceWatcher recommended for flexibility) ---
        // Simplified FindAllAsync for now:
        auto selector = GattDeviceService::GetDeviceSelectorFromUuid(g_hrServiceUuid);
        auto devices = DeviceInformation::FindAllAsync(selector).get();
        if (devices.Size() == 0) {
            throw std::runtime_error("No HR device found.");
        }
        auto deviceInfo = devices.GetAt(0); // Still using first device here

        ReportStatus(2, "Connecting..."); // Status: Connecting
        bleDevice = BluetoothLEDevice::FromIdAsync(deviceInfo.Id()).get();
        if (!bleDevice) {
            throw std::runtime_error("Failed to get BluetoothLEDevice object.");
        }

        // Monitor connection status
        connectionStatusToken = bleDevice.ConnectionStatusChanged([&](BluetoothLEDevice const& device, auto const& args) {
            if (device.ConnectionStatus() == BluetoothConnectionStatus::Disconnected) {
                ReportStatus(5, "Device Disconnected"); // Status: Disconnected
                // Signal stop or attempt reconnect based on desired logic
                g_shouldStop = true; // Example: Stop on disconnect
            }
            });

        // Check initial connection status (FromIdAsync doesn't guarantee connection)
        if (bleDevice.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
            // Optional: May need explicit connect call depending on device/scenario
            // GattSession::FromDeviceIdAsync might be more robust for session management
            ReportStatus(2, "Waiting for Connection...");
            // Add logic to wait or fail if not connected after timeout
        }

        ReportStatus(3, "Discovering Services..."); // Status: Discovering
        auto serviceResult = bleDevice.GetGattServicesForUuidAsync(g_hrServiceUuid).get();
        if (serviceResult.Status() != GattCommunicationStatus::Success || serviceResult.Services().Size() == 0) {
            throw std::runtime_error("HR Service not found.");
        }
        auto hrService = serviceResult.Services().GetAt(0);

        auto charResult = hrService.GetCharacteristicsForUuidAsync(g_hrMeasurementUuid).get();
        if (charResult.Status() != GattCommunicationStatus::Success || charResult.Characteristics().Size() == 0) {
            throw std::runtime_error("HR Measurement Characteristic not found.");
        }
        hrCharacteristic = charResult.Characteristics().GetAt(0);

        ReportStatus(4, "Subscribing..."); // Status: Subscribing
        auto status = hrCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
            GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
        if (status != GattCommunicationStatus::Success) {
            throw std::runtime_error("Failed to subscribe to HR notifications.");
        }

        valueChangedToken = hrCharacteristic.ValueChanged(
            [&](GattCharacteristic const& sender, GattValueChangedEventArgs const& args) {
                try {
                    DataReader reader = DataReader::FromBuffer(args.CharacteristicValue());
                    uint8_t flags = reader.ReadByte();
                    uint16_t rate = (flags & 0x01) ? reader.ReadUInt16() : reader.ReadByte();
                    ReportHeartRate(rate);
                }
                catch (winrt::hresult_error const& e) {
                    // Handle read error if buffer is malformed etc.
                    std::string errorMsg = "HR Read Error: " + winrt::to_string(e.message());
                    ReportStatus(99, errorMsg.c_str()); // Status: Runtime Error
                }
            });

        ReportStatus(10, "Connected and Monitoring"); // Status: Connected/Streaming


        while (!g_shouldStop) {
            // Use condition variable or just sleep
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        ReportStatus(11, "Stopping..."); // Status: Stopping

    }
    catch (winrt::hresult_error const& e) {
        std::string errorMsg = "BLE Error: " + winrt::to_string(e.message());
        ReportStatus(99, errorMsg.c_str()); // Status: Error
    }
    catch (std::exception const& e) {
        std::string errorMsg = "Std Error: " + std::string(e.what());
        ReportStatus(99, errorMsg.c_str()); // Status: Error
    }
    catch (...) {
        ReportStatus(99, "Unknown error occurred."); // Status: Error
    }

    // --- Cleanup (happens within worker thread) ---
    try {
        if (hrCharacteristic) {
            hrCharacteristic.ValueChanged(valueChangedToken); // Unsubscribe event
            // Attempt to disable notifications (best effort)
            hrCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::None).get(); // Still blocking, but on worker thread
        }
        if (bleDevice) {
            bleDevice.ConnectionStatusChanged(connectionStatusToken); // Unsubscribe event
            bleDevice.Close(); // Close connection and release resources
        }
    }
    catch (winrt::hresult_error const& e) {
        // Log cleanup error, but proceed
        std::string errorMsg = "Cleanup Error: " + winrt::to_string(e.message());
        ReportStatus(98, errorMsg.c_str()); // Status: Cleanup Error
    }
    catch (...) {
        ReportStatus(98, "Unknown cleanup error.");
    }

    bleDevice = nullptr; // Release WinRT objects
    hrCharacteristic = nullptr;

    ReportStatus(0, "Stopped"); // Status: Idle/Stopped
    uninit_apartment(); // Uninitialize COM/WinRT for this thread
}

// Helper to safely invoke status callback
void ReportStatus(int status, const char* message) {
    g_currentState = status;
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    if (g_statusCallback) {
        g_statusCallback(status, message);
    }
    // Optional: Log to debug output/file here too
    // OutputDebugStringA(...)
}

// Helper to safely invoke HR callback
void ReportHeartRate(int bpm) {
    // No need to store bpm globally if using callback
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    if (g_hrCallback) {
        g_hrCallback(bpm);
    }
}

// --- Exported C API ---
extern "C" {
    __declspec(dllexport) int InitializePlugin() {
        // Any one-time setup if needed
        g_currentState = 0;
        return 0; // Success
    }

    __declspec(dllexport) int RegisterStatusCallback(StatusCallback callback) {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        g_statusCallback = callback;
        return 0;
    }

    __declspec(dllexport) int RegisterHeartRateCallback(HeartRateCallback callback) {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        g_hrCallback = callback;
        return 0;
    }

    __declspec(dllexport) int StartHrMonitoring() {
        if (g_workerThread.joinable()) {
            return -1; // Already running
        }
        g_shouldStop = false;
        try {
            // Start thread using std::thread
            g_workerThread = std::thread(BleWorkerLogic);
        }
        catch (std::exception const& e) {
            // Failed to create thread?
            ReportStatus(99, e.what());
            return -2; // Thread creation failed
        }
        return 0; // Success (thread started)
    }

    __declspec(dllexport) int StopHrMonitoring() {
        if (!g_workerThread.joinable()) {
            return -1; // Not running
        }
        g_shouldStop = true; // Signal worker thread

        // Wait for worker thread to finish
        try {
            if (g_workerThread.joinable()) {
                g_workerThread.join(); // Waits indefinitely
            }
        }
        catch (std::system_error const& e) {
            // Error joining thread?
            ReportStatus(98, e.what());
            return -2;
        }
        // g_workerThread destructor handles cleanup if needed, join ensures it's done.
        return 0; // Success
    }

    //// Optional: Keep GetLatestStatus if needed, but callback is better
    __declspec(dllexport) int GetCurrentStatus() {
        return g_currentState.load();
    }

    // Remove GetLatestHeartRate if relying solely on callback
    // __declspec(dllexport) int GetLatestHeartRate() { ... }
}



//#include "pch.h"
//#include <windows.h>
//#include <winrt/Windows.Foundation.h>
//#include <winrt/Windows.Devices.Bluetooth.h>
//#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
//#include <winrt/Windows.Devices.Enumeration.h>
//#include <winrt/Windows.Storage.Streams.h>
//#include <atomic>
//#include <mutex>
//#include <winrt/Windows.Foundation.Collections.h>
//
//using namespace winrt;
//using namespace Windows::Devices::Bluetooth;
//using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
//using namespace Windows::Devices::Enumeration;
//using namespace Windows::Storage::Streams;
//
//// Global variables for thread-safe data sharing
//std::atomic<int> g_heartRate(0);
//std::atomic<bool> g_shouldStop(false);
//std::mutex g_bleMutex;
//HANDLE g_workerThread = nullptr;
//
//// BLE Components (protected by mutex)
//winrt::guid g_hrServiceUuid{ 0x0000180D, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB} };
//winrt::guid g_hrMeasurementUuid{ 0x00002A37, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB} };
//BluetoothLEDevice g_bleDevice = nullptr;
//GattCharacteristic g_hrCharacteristic = nullptr;
//
//DWORD WINAPI BleWorkerThread(LPVOID lpParam)
//{
//    // **Use multi-threaded apartment** instead of single-threaded
//    init_apartment(apartment_type::multi_threaded);
//
//    try {
//        // --- Perform the same BLE device/characteristic discovery ---
//        auto selector = GattDeviceService::GetDeviceSelectorFromUuid(g_hrServiceUuid);
//        auto devices = DeviceInformation::FindAllAsync(selector).get();
//        if (devices.Size() == 0) {
//            return 27;
//        }
//
//        auto deviceInfo = devices.GetAt(0);
//
//        {
//            std::lock_guard<std::mutex> lock(g_bleMutex);
//            g_bleDevice = BluetoothLEDevice::FromIdAsync(deviceInfo.Id()).get();
//        }
//
//        if (!g_bleDevice) {
//            return 2;
//        }
//
//        auto serviceResult = g_bleDevice.GetGattServicesForUuidAsync(g_hrServiceUuid).get();
//        if (serviceResult.Status() != GattCommunicationStatus::Success || serviceResult.Services().Size() == 0) {
//            return 3;
//        }
//        auto hrService = serviceResult.Services().GetAt(0);
//
//        auto charResult = hrService.GetCharacteristicsForUuidAsync(g_hrMeasurementUuid).get();
//        if (charResult.Status() != GattCommunicationStatus::Success || charResult.Characteristics().Size() == 0) {
//            return 4;
//        }
//
//        {
//            std::lock_guard<std::mutex> lock(g_bleMutex);
//            g_hrCharacteristic = charResult.Characteristics().GetAt(0);
//        }
//
//        auto status = g_hrCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
//        if (status != GattCommunicationStatus::Success) {
//            return 5;
//        }
//
//        // Subscribe to notifications
//        g_hrCharacteristic.ValueChanged(
//            [](auto&&, GattValueChangedEventArgs const& args)
//            {
//                DataReader reader = DataReader::FromBuffer(args.CharacteristicValue());
//                uint8_t flags = reader.ReadByte();
//                uint16_t rate = (flags & 0x01) ? reader.ReadUInt16() : reader.ReadByte();
//                g_heartRate = rate;
//            }
//        );
//
//        // keep thread alive idk, källa: deepseak
//        while (!g_shouldStop)
//        {
//            ::Sleep(100);
//        }
//
//        /*
//        // Cleanup
//        if (g_hrCharacteristic) {
//            g_hrCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
//                GattClientCharacteristicConfigurationDescriptorValue::None).get();
//        }
//
//        if (g_bleDevice)
//        {
//            g_bleDevice.Close();
//        }
//        uninit_apartment();
//
//        {
//            std::lock_guard<std::mutex> lock(g_bleMutex);
//            g_hrCharacteristic = nullptr;
//            g_bleDevice = nullptr;
//        }
//        */
//    }
//    catch (...)
//    {
//        //uninit_apartment();
//        return 6;
//    }
//
//    //uninit_apartment();
//    return 0;
//}
//
//
//extern "C" {
//    __declspec(dllexport) void StartHrMonitoring() {
//        if (g_workerThread) return;
//        g_shouldStop = false;
//        g_workerThread = CreateThread(NULL, 0, BleWorkerThread, NULL, 0, NULL);
//    }
//
//    __declspec(dllexport) int StopHrMonitoring() {
//        
//        if (!g_workerThread) return 45;
//        g_shouldStop = true;
//        
//        // Force cleanup regardless of thread state
//        /*
//        {
//            
//            std::lock_guard<std::mutex> lock(g_bleMutex);
//            if (g_hrCharacteristic) {
//                g_hrCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
//                    GattClientCharacteristicConfigurationDescriptorValue::None).get();
//                g_hrCharacteristic = nullptr;
//            }
//            if (g_bleDevice) {
//                g_bleDevice.Close();
//                g_bleDevice = nullptr;
//            }
//        }
//
//        WaitForSingleObject(g_workerThread, 5000); // Wait up to 5 seconds
//        CloseHandle(g_workerThread);
//        g_workerThread = nullptr;
//        */
//        return 87;
//    }
//
//    __declspec(dllexport) int GetLatestHeartRate() {
//        return g_heartRate.load();
//    }
//}
