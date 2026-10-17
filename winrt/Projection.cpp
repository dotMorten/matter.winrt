#include "ProjectionTypes.h"
#include "CameraVideoStreamAllocation.h"
#include "CameraAudioAllocationOwnership.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/CommandSender.h>
#include <app/BufferedReadCallback.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/WriteClient.h>
#include <app/data-model/EncodableToTLV.h>
#include <app/server-cluster/testing/EmptyProvider.h>
#include <controller/CHIPCluster.h>
#include <controller/CHIPDeviceController.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/ExampleOperationalCredentialsIssuer.h>
#include <controller/InvokeInteraction.h>
#include <controller/CHIPControllerBuildConfig.h>
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
#include "CameraControllerRuntime.h"
#endif
#include <credentials/GroupDataProviderImpl.h>
#include <credentials/PersistentStorageOpCertStore.h>
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <crypto/CHIPCryptoPAL.h>
#include <crypto/PersistentStorageOperationalKeystore.h>
#include <crypto/RawKeySessionKeystore.h>
#include <inet/InetInterface.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/TestGroupData.h>
#include <lib/core/TLV.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/KvsPersistentStorageDelegate.h>
#include <platform/Windows/BLEManagerImpl.h>
#include <platform/Windows/ConfigurationManagerImpl.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>
#include <winrt/Windows.System.Threading.h>

#include <chrono>
#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <type_traits>
#include <utility>
#include <vector>

namespace winrt::Matter::Windows::Controller::implementation {
namespace {

using namespace chip;
using namespace chip::Controller;
using namespace chip::DeviceLayer;

constexpr FabricId kControllerFabricId = 1;
constexpr auto kCommissioningTimeout    = std::chrono::seconds(120);
constexpr auto kCommissioningFailureDetailTimeout = std::chrono::milliseconds(100);
constexpr auto kInteractionTimeout      = std::chrono::seconds(30);
constexpr auto kTimedInteractionMargin  = std::chrono::seconds(5);
constexpr char kCommissionedNodesKey[] = "winrt/nodes";
constexpr size_t kGenericTlvBufferSize = 64 * 1024;
constexpr size_t kMaximumPendingReports = 256;

std::unique_lock<std::timed_mutex> AcquireOperationLock(
    std::timed_mutex & mutex, std::shared_ptr<std::atomic_bool> const & cancellation,
    std::chrono::steady_clock::time_point deadline, std::atomic_bool const * closing = nullptr)
{
    std::unique_lock lock(mutex, std::defer_lock);
    for (;;)
    {
        if (closing && closing->load(std::memory_order_acquire))
        {
            throw hresult_illegal_method_call(L"The Matter controller is closing.");
        }
        if (cancellation && cancellation->load(std::memory_order_acquire))
        {
            throw hresult_canceled();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter operation admission timed out.");
        }
        if (lock.owns_lock())
        {
            return lock;
        }
        (void) lock.try_lock_until(std::min(deadline, now + std::chrono::milliseconds(100)));
    }
}

class RuntimeOperationAdmission
{
public:
    struct NodeLease
    {
        // Destruction releases the node lock before its mutex and the runtime lifetime lease.
        std::shared_lock<std::shared_timed_mutex> lifetime;
        std::shared_ptr<std::timed_mutex> nodeMutex;
        std::unique_lock<std::timed_mutex> nodeLock;
    };

    std::shared_lock<std::shared_timed_mutex> AcquireLifetime(
        std::shared_ptr<std::atomic_bool> const & cancellation = {},
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kInteractionTimeout)
    {
        std::shared_lock lease(mLifetimeMutex, std::defer_lock);
        WaitForLifetime(lease, cancellation, deadline);
        return lease;
    }

    NodeLease AcquireNode(uint64_t node, std::shared_ptr<std::atomic_bool> const & cancellation = {},
                         std::chrono::steady_clock::time_point deadline =
                             std::chrono::steady_clock::now() + kInteractionTimeout)
    {
        auto lifetime = AcquireLifetime(cancellation, deadline);
        return AcquireNodeWithLifetime(std::move(lifetime), node, cancellation, deadline);
    }

    NodeLease AcquireNodeWithLifetime(std::shared_lock<std::shared_timed_mutex> lifetime, uint64_t node,
                                     std::shared_ptr<std::atomic_bool> const & cancellation,
                                     std::chrono::steady_clock::time_point deadline)
    {
        std::shared_ptr<std::timed_mutex> nodeMutex;
        {
            std::scoped_lock lock(mIndexMutex);
            for (auto entry = mNodeMutexes.begin(); entry != mNodeMutexes.end();)
            {
                if (entry->second.expired())
                {
                    entry = mNodeMutexes.erase(entry);
                }
                else
                {
                    ++entry;
                }
            }
            auto & weak = mNodeMutexes[node];
            nodeMutex = weak.lock();
            if (!nodeMutex)
            {
                nodeMutex = std::make_shared<std::timed_mutex>();
                weak = nodeMutex;
            }
        }
        auto nodeLock = AcquireOperationLock(*nodeMutex, cancellation, deadline, &mClosing);
        return { std::move(lifetime), std::move(nodeMutex), std::move(nodeLock) };
    }

    std::unique_lock<std::shared_timed_mutex> AcquireExclusive(
        std::shared_ptr<std::atomic_bool> const & cancellation = {},
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kCommissioningTimeout)
    {
        std::unique_lock lease(mLifetimeMutex, std::defer_lock);
        WaitForLifetime(lease, cancellation, deadline);
        return lease;
    }

    std::unique_lock<std::shared_timed_mutex> BeginShutdown()
    {
        mClosing.store(true, std::memory_order_release);
        return std::unique_lock(mLifetimeMutex);
    }

    std::unique_lock<std::timed_mutex> AcquireGate(
        std::timed_mutex & mutex, std::shared_ptr<std::atomic_bool> const & cancellation,
        std::chrono::steady_clock::time_point deadline)
    {
        return AcquireOperationLock(mutex, cancellation, deadline, &mClosing);
    }

    std::shared_lock<std::shared_timed_mutex> AcquireCleanup()
    {
        std::shared_lock lease(mLifetimeMutex, std::defer_lock);
        if (!mClosing.load(std::memory_order_acquire))
        {
            lease.lock();
            if (!mClosing.load(std::memory_order_acquire))
            {
                return lease;
            }
            lease.unlock();
        }
        return lease;
    }

private:
    template <typename Lock>
    void WaitForLifetime(Lock & lease, std::shared_ptr<std::atomic_bool> const & cancellation,
                         std::chrono::steady_clock::time_point deadline)
    {
        for (;;)
        {
            if (mClosing.load(std::memory_order_acquire))
            {
                throw hresult_illegal_method_call(L"The Matter controller is closing.");
            }
            if (cancellation && cancellation->load(std::memory_order_acquire))
            {
                throw hresult_canceled();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter operation admission timed out.");
            }
            if (lease.owns_lock())
            {
                return;
            }
            (void) lease.try_lock_until(std::min(deadline, now + std::chrono::milliseconds(100)));
        }
    }

    std::shared_timed_mutex mLifetimeMutex;
    std::atomic_bool mClosing{ false };
    std::mutex mIndexMutex;
    std::unordered_map<uint64_t, std::weak_ptr<std::timed_mutex>> mNodeMutexes;
};

struct NativeCommissioningProgress
{
    chip::Controller::CommissioningStage stage;
    CHIP_ERROR error = CHIP_NO_ERROR;
    bool completed   = false;
    bool retrying    = false;
};

using CommissioningProgressCallback = std::function<void(const NativeCommissioningProgress &)>;

Controller::MatterCommissioningStage ProjectCommissioningStage(chip::Controller::CommissioningStage stage)
{
    switch (stage)
    {
    case chip::Controller::CommissioningStage::kSecurePairing:
        return Controller::MatterCommissioningStage::EstablishingPase;
    case chip::Controller::CommissioningStage::kReadCommissioningInfo:
        return Controller::MatterCommissioningStage::ReadingCommissioningInformation;
    case chip::Controller::CommissioningStage::kConfigRegulatory:
        return Controller::MatterCommissioningStage::ConfiguringRegulatoryInformation;
    case chip::Controller::CommissioningStage::kWiFiNetworkSetup:
    case chip::Controller::CommissioningStage::kThreadNetworkSetup:
    case chip::Controller::CommissioningStage::kRequestWiFiCredentials:
    case chip::Controller::CommissioningStage::kRequestThreadCredentials:
        return Controller::MatterCommissioningStage::ProvisioningNetwork;
    case chip::Controller::CommissioningStage::kWiFiNetworkEnable:
    case chip::Controller::CommissioningStage::kThreadNetworkEnable:
        return Controller::MatterCommissioningStage::ConnectingDeviceToNetwork;
    case chip::Controller::CommissioningStage::kFindOperationalForStayActive:
    case chip::Controller::CommissioningStage::kFindOperationalForCommissioningComplete:
        return Controller::MatterCommissioningStage::DiscoveringOperationalDevice;
    case chip::Controller::CommissioningStage::kSendComplete:
        return Controller::MatterCommissioningStage::SendingCommissioningComplete;
    default:
        return Controller::MatterCommissioningStage::Unknown;
    }
}

Controller::MatterCommissioningTransport ProjectCommissioningTransport(chip::Controller::CommissioningStage stage)
{
    if (stage == chip::Controller::CommissioningStage::kFindOperationalForStayActive ||
        stage == chip::Controller::CommissioningStage::kFindOperationalForCommissioningComplete ||
        stage == chip::Controller::CommissioningStage::kSendComplete)
    {
        return Controller::MatterCommissioningTransport::OperationalIp;
    }
    if (stage == chip::Controller::CommissioningStage::kWiFiNetworkSetup ||
        stage == chip::Controller::CommissioningStage::kWiFiNetworkEnable)
    {
        return Controller::MatterCommissioningTransport::WiFi;
    }
    if (stage == chip::Controller::CommissioningStage::kThreadNetworkSetup ||
        stage == chip::Controller::CommissioningStage::kThreadNetworkEnable)
    {
        return Controller::MatterCommissioningTransport::Thread;
    }
    return Controller::MatterCommissioningTransport::Bluetooth;
}

Controller::MatterCommissioningFailureKind ClassifyCommissioningFailure(Controller::MatterCommissioningStage stage,
                                                                        bool timedOut)
{
    switch (stage)
    {
    case Controller::MatterCommissioningStage::SearchingForDevice:
        return Controller::MatterCommissioningFailureKind::DeviceNotFoundOverBluetooth;
    case Controller::MatterCommissioningStage::ConnectingOverBluetooth:
        return Controller::MatterCommissioningFailureKind::BluetoothConnectionFailed;
    case Controller::MatterCommissioningStage::EstablishingPase:
        return Controller::MatterCommissioningFailureKind::PaseFailed;
    case Controller::MatterCommissioningStage::ProvisioningNetwork:
        return Controller::MatterCommissioningFailureKind::NetworkCredentialsRejected;
    case Controller::MatterCommissioningStage::ConnectingDeviceToNetwork:
        return Controller::MatterCommissioningFailureKind::DeviceNetworkConnectionFailed;
    case Controller::MatterCommissioningStage::DiscoveringOperationalDevice:
        return Controller::MatterCommissioningFailureKind::OperationalDiscoveryTimedOut;
    case Controller::MatterCommissioningStage::EstablishingCase:
        return Controller::MatterCommissioningFailureKind::CaseEstablishmentFailed;
    case Controller::MatterCommissioningStage::SendingCommissioningComplete:
        return Controller::MatterCommissioningFailureKind::CommissioningCompleteFailed;
    default:
        return timedOut ? Controller::MatterCommissioningFailureKind::OverallTimeout
                        : Controller::MatterCommissioningFailureKind::Unknown;
    }
}

std::vector<uint8_t> Utf8Bytes(hstring const & value)
{
    std::string encoded = to_string(value);
    std::vector<uint8_t> bytes(encoded.begin(), encoded.end());
    if (!encoded.empty())
    {
        Crypto::ClearSecretData(reinterpret_cast<uint8_t *>(encoded.data()), encoded.size());
    }
    return bytes;
}

std::chrono::milliseconds InteractionWaitTimeout(std::optional<uint16_t> timedInteractionTimeout)
{
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kInteractionTimeout);
    if (timedInteractionTimeout.has_value())
    {
        timeout += std::chrono::milliseconds(*timedInteractionTimeout) + kTimedInteractionMargin;
    }
    return timeout;
}

class ChipOperationError : public hresult_error
{
public:
    ChipOperationError(CHIP_ERROR error, hstring message) :
        hresult_error(HRESULT_FROM_WIN32(ERROR_GEN_FAILURE), message), mNativeError(error)
    {}
    CHIP_ERROR NativeError() const { return mNativeError; }
private:
    CHIP_ERROR mNativeError;
};

[[noreturn]] void ThrowChipError(CHIP_ERROR error, wchar_t const * operation)
{
    std::wstring message(operation);
    message.append(L": ");
    message.append(to_hstring(error.AsString()).c_str());
    throw ChipOperationError(error, hstring(message));
}

void CheckChipError(CHIP_ERROR error, wchar_t const * operation)
{
    if (error != CHIP_NO_ERROR)
    {
        ThrowChipError(error, operation);
    }
}

CHIP_ERROR EncodeInspectable(TLV::TLVWriter & writer, TLV::Tag tag, Windows::Foundation::IInspectable const & value);

CHIP_ERROR EncodePropertySet(TLV::TLVWriter & writer, TLV::Tag tag,
                             Windows::Foundation::Collections::IPropertySet const & values)
{
    TLV::TLVType container;
    ReturnErrorOnFailure(writer.StartContainer(tag, TLV::kTLVType_Structure, container));
    std::vector<std::pair<uint8_t, Windows::Foundation::IInspectable>> fields;
    for (auto const & entry : values)
    {
        std::string key = to_string(entry.Key());
        char * end      = nullptr;
        unsigned long number = std::strtoul(key.c_str(), &end, 10);
        VerifyOrReturnError(end != key.c_str() && *end == '\0' && number <= UINT8_MAX, CHIP_ERROR_INVALID_ARGUMENT);
        fields.emplace_back(static_cast<uint8_t>(number), entry.Value());
    }
    std::sort(fields.begin(), fields.end(), [](auto const & left, auto const & right) { return left.first < right.first; });
    for (auto const & field : fields)
    {
        ReturnErrorOnFailure(EncodeInspectable(writer, TLV::ContextTag(field.first), field.second));
    }
    return writer.EndContainer(container);
}

CHIP_ERROR EncodeInspectable(TLV::TLVWriter & writer, TLV::Tag tag, Windows::Foundation::IInspectable const & value)
{
    if (!value)
    {
        return writer.PutNull(tag);
    }
    if (auto propertySet = value.try_as<Windows::Foundation::Collections::IPropertySet>())
    {
        return EncodePropertySet(writer, tag, propertySet);
    }
    if (auto inspectableVector = value.try_as<Windows::Foundation::Collections::IVector<Windows::Foundation::IInspectable>>())
    {
        TLV::TLVType container;
        ReturnErrorOnFailure(writer.StartContainer(tag, TLV::kTLVType_Array, container));
        for (auto const & item : inspectableVector)
        {
            ReturnErrorOnFailure(EncodeInspectable(writer, TLV::AnonymousTag(), item));
        }
        return writer.EndContainer(container);
    }
    if (auto byteVector = value.try_as<Windows::Foundation::Collections::IVector<uint8_t>>())
    {
        std::vector<uint8_t> bytes(byteVector.Size());
        byteVector.GetMany(0, bytes);
        return writer.PutBytes(tag, bytes.data(), static_cast<uint32_t>(bytes.size()));
    }

    auto propertyValue = value.try_as<Windows::Foundation::IPropertyValue>();
    VerifyOrReturnError(propertyValue, CHIP_ERROR_INVALID_ARGUMENT);
    switch (propertyValue.Type())
    {
    case Windows::Foundation::PropertyType::Boolean:
        return writer.Put(tag, propertyValue.GetBoolean());
    case Windows::Foundation::PropertyType::Int16:
        return writer.Put(tag, propertyValue.GetInt16());
    case Windows::Foundation::PropertyType::Int32:
        return writer.Put(tag, propertyValue.GetInt32());
    case Windows::Foundation::PropertyType::Int64:
        return writer.Put(tag, propertyValue.GetInt64());
    case Windows::Foundation::PropertyType::UInt8:
        return writer.Put(tag, propertyValue.GetUInt8());
    case Windows::Foundation::PropertyType::UInt16:
        return writer.Put(tag, propertyValue.GetUInt16());
    case Windows::Foundation::PropertyType::UInt32:
        return writer.Put(tag, propertyValue.GetUInt32());
    case Windows::Foundation::PropertyType::UInt64:
        return writer.Put(tag, propertyValue.GetUInt64());
    case Windows::Foundation::PropertyType::Single:
        return writer.Put(tag, propertyValue.GetSingle());
    case Windows::Foundation::PropertyType::Double:
        return writer.Put(tag, propertyValue.GetDouble());
    case Windows::Foundation::PropertyType::String: {
        std::string text = to_string(propertyValue.GetString());
        return writer.PutString(tag, CharSpan(text.data(), text.size()));
    }
    case Windows::Foundation::PropertyType::UInt8Array: {
        com_array<uint8_t> bytes;
        propertyValue.GetUInt8Array(bytes);
        return writer.PutBytes(tag, bytes.data(), static_cast<uint32_t>(bytes.size()));
    }
    default:
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
}

CHIP_ERROR EncodePropertySetRoot(Windows::Foundation::Collections::IPropertySet const & values, std::vector<uint8_t> & encoded)
{
    VerifyOrReturnError(values, CHIP_ERROR_INVALID_ARGUMENT);
    encoded.resize(kGenericTlvBufferSize);
    TLV::TLVWriter writer;
    writer.Init(encoded.data(), encoded.size());
    if (values.HasKey(L"value"))
    {
        VerifyOrReturnError(values.Size() == 1, CHIP_ERROR_INVALID_ARGUMENT);
        ReturnErrorOnFailure(EncodeInspectable(writer, TLV::AnonymousTag(), values.Lookup(L"value")));
    }
    else
    {
        ReturnErrorOnFailure(EncodePropertySet(writer, TLV::AnonymousTag(), values));
    }
    ReturnErrorOnFailure(writer.Finalize());
    encoded.resize(writer.GetLengthWritten());
    return CHIP_NO_ERROR;
}

CHIP_ERROR DecodeTlvValue(TLV::TLVReader & reader, Windows::Foundation::IInspectable & value)
{
    switch (reader.GetType())
    {
    case TLV::kTLVType_Boolean: {
        bool decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        value = box_value(decoded);
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_SignedInteger: {
        int64_t decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        value = box_value(decoded);
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_UnsignedInteger: {
        uint64_t decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        value = box_value(decoded);
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_FloatingPointNumber: {
        double decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        value = box_value(decoded);
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_UTF8String: {
        CharSpan decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        value = box_value(to_hstring(std::string(decoded.data(), decoded.size())));
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_ByteString: {
        ByteSpan decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        std::vector<uint8_t> bytes(decoded.begin(), decoded.end());
        value = single_threaded_vector(std::move(bytes)).as<Windows::Foundation::IInspectable>();
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_Null:
        value = nullptr;
        return CHIP_NO_ERROR;
    case TLV::kTLVType_Structure: {
        Windows::Foundation::Collections::PropertySet properties;
        TLV::TLVType container;
        ReturnErrorOnFailure(reader.EnterContainer(container));
        CHIP_ERROR error;
        while ((error = reader.Next()) == CHIP_NO_ERROR)
        {
            VerifyOrReturnError(TLV::IsContextTag(reader.GetTag()), CHIP_ERROR_INVALID_TLV_TAG);
            const auto fieldTag = reader.GetTag();
            Windows::Foundation::IInspectable field;
            ReturnErrorOnFailure(DecodeTlvValue(reader, field));
            properties.Insert(to_hstring(TLV::TagNumFromTag(fieldTag)), field);
        }
        VerifyOrReturnError(error == CHIP_END_OF_TLV, error);
        ReturnErrorOnFailure(reader.ExitContainer(container));
        value = properties;
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_Array:
    case TLV::kTLVType_List: {
        auto items = single_threaded_vector<Windows::Foundation::IInspectable>();
        TLV::TLVType container;
        ReturnErrorOnFailure(reader.EnterContainer(container));
        CHIP_ERROR error;
        while ((error = reader.Next()) == CHIP_NO_ERROR)
        {
            Windows::Foundation::IInspectable item;
            ReturnErrorOnFailure(DecodeTlvValue(reader, item));
            items.Append(item);
        }
        VerifyOrReturnError(error == CHIP_END_OF_TLV, error);
        ReturnErrorOnFailure(reader.ExitContainer(container));
        value = items.as<Windows::Foundation::IInspectable>();
        return CHIP_NO_ERROR;
    }
    default:
        return CHIP_ERROR_WRONG_TLV_TYPE;
    }
}

Windows::Foundation::Collections::IPropertySet DecodePropertySetRoot(TLV::TLVReader & reader)
{
    Windows::Foundation::IInspectable decoded;
    CheckChipError(DecodeTlvValue(reader, decoded), L"Decode Matter TLV");
    if (auto properties = decoded.try_as<Windows::Foundation::Collections::IPropertySet>())
    {
        return properties;
    }
    Windows::Foundation::Collections::PropertySet result;
    result.Insert(L"value", decoded);
    return result;
}

template <typename Value>
Value PropertySetScalar(Windows::Foundation::Collections::IPropertySet const & values, wchar_t const * description)
{
    if (!values || !values.HasKey(L"value") || !values.Lookup(L"value"))
    {
        throw hresult_error(E_BOUNDS, hstring(description) + L" is null or missing.");
    }
    try
    {
        if constexpr (std::is_integral_v<Value> && std::is_unsigned_v<Value> && !std::is_same_v<Value, bool>)
        {
            uint64_t decoded = unbox_value<uint64_t>(values.Lookup(L"value"));
            if (decoded > std::numeric_limits<Value>::max())
            {
                throw hresult_error(E_BOUNDS, hstring(description) + L" is outside the projected numeric range.");
            }
            return static_cast<Value>(decoded);
        }
        else if constexpr (std::is_integral_v<Value> && std::is_signed_v<Value>)
        {
            int64_t decoded = unbox_value<int64_t>(values.Lookup(L"value"));
            if (decoded < std::numeric_limits<Value>::min() || decoded > std::numeric_limits<Value>::max())
            {
                throw hresult_error(E_BOUNDS, hstring(description) + L" is outside the projected numeric range.");
            }
            return static_cast<Value>(decoded);
        }
        else
        {
            return unbox_value<Value>(values.Lookup(L"value"));
        }
    }
    catch (hresult_error const &)
    {
        throw hresult_error(E_INVALIDARG, hstring(description) + L" has an unexpected Matter type.");
    }
}

struct PairingState
{
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    Optional<chip::Controller::CommissioningStage> activeStage;
    Optional<chip::Controller::CommissioningStage> lastCompletedStage;
    CompletionStatus completionStatus;
    bool hasCompletionStatus = false;
    CommissioningProgressCallback progressCallback;
};

void AppendStage(std::wstring & message, wchar_t const * label, Optional<chip::Controller::CommissioningStage> stage)
{
    if (!stage.HasValue())
    {
        return;
    }

    chip::Controller::CommissioningStage value = stage.Value();
    char const * name                           = StageToString(value);
    message.append(L" ");
    message.append(label);
    message.append(L": ");
    if (name != nullptr && name[0] != '\0')
    {
        message.append(to_hstring(name).c_str());
        message.append(L" (");
        message.append(std::to_wstring(static_cast<uint16_t>(value)));
        message.append(L")");
    }
    else
    {
        message.append(std::to_wstring(static_cast<uint16_t>(value)));
    }
    message.append(L".");
}

std::wstring CommissioningTimeoutMessage(PairingState const & state)
{
    std::wstring message = L"Matter commissioning timed out.";
    AppendStage(message, L"Active stage", state.activeStage);
    AppendStage(message, L"Last completed stage", state.lastCompletedStage);
    return message;
}

void AppendDeviceDebugText(std::wstring & message, wchar_t const * label, std::string const & text)
{
    if (!text.empty())
    {
        message.append(L" ");
        message.append(label);
        message.append(L": ");
        message.append(to_hstring(text).c_str());
        message.append(L".");
    }
}

[[noreturn]] void ThrowCommissioningError(PairingState const & state)
{
    std::wstring message = L"Commission device: ";
    message.append(to_hstring(state.error.AsString()).c_str());
    message.append(L".");
    AppendStage(message, L"Failed stage", state.completionStatus.failedStage);
    AppendStage(message, L"Active stage", state.activeStage);
    AppendStage(message, L"Last completed stage", state.lastCompletedStage);

    if (state.completionStatus.commissioningError.HasValue())
    {
        message.append(L" Commissioning error: ");
        message.append(std::to_wstring(static_cast<uint8_t>(state.completionStatus.commissioningError.Value())));
        message.append(L".");
    }
    if (state.completionStatus.networkCommissioningStatus.HasValue())
    {
        message.append(L" Network commissioning status: ");
        message.append(std::to_wstring(static_cast<uint8_t>(state.completionStatus.networkCommissioningStatus.Value())));
        message.append(L".");
    }
    if (state.completionStatus.connectNetworkErrorValue.HasValue())
    {
        message.append(L" Connect network error: ");
        message.append(std::to_wstring(state.completionStatus.connectNetworkErrorValue.Value()));
        message.append(L".");
    }
    if (state.completionStatus.operationalCertStatus.HasValue())
    {
        message.append(L" Operational certificate status: ");
        message.append(std::to_wstring(static_cast<uint8_t>(state.completionStatus.operationalCertStatus.Value())));
        message.append(L".");
    }
    if (state.completionStatus.attestationResult.HasValue())
    {
        message.append(L" Attestation result: ");
        message.append(std::to_wstring(static_cast<uint16_t>(state.completionStatus.attestationResult.Value())));
        message.append(L".");
    }
    AppendDeviceDebugText(message, L"Commissioning debug text", state.completionStatus.commissioningDebugText);
    AppendDeviceDebugText(message, L"Network commissioning debug text", state.completionStatus.networkCommissioningDebugText);
    throw ChipOperationError(state.error, hstring(message));
}

class PairingDelegate final : public DevicePairingDelegate, public Credentials::DeviceAttestationDelegate
{
public:
    PairingDelegate(PairingState & state, bool allowTestAttestation) : mState(state), mAllowTestAttestation(allowTestAttestation) {}

    void Reset(CommissioningProgressCallback progressCallback = {})
    {
        std::scoped_lock lock(mState.mutex);
        mState.complete               = false;
        mState.error                  = CHIP_NO_ERROR;
        mState.activeStage            = NullOptional;
        mState.lastCompletedStage     = NullOptional;
        mState.completionStatus       = CompletionStatus();
        mState.hasCompletionStatus    = false;
        mState.progressCallback       = std::move(progressCallback);
    }

    void OnCommissioningComplete(NodeId, CHIP_ERROR error) override
    {
        Complete(error);
    }

    void OnCommissioningFailure(PeerId, const CompletionStatus & completionStatus) override
    {
        CommissioningProgressCallback callback;
        {
            std::scoped_lock lock(mState.mutex);
            mState.completionStatus    = completionStatus;
            mState.hasCompletionStatus = true;
            mState.error               = completionStatus.err;
            mState.complete            = true;
            callback                   = mState.progressCallback;
        }
        if (callback && completionStatus.failedStage.HasValue())
        {
            callback({ completionStatus.failedStage.Value(), completionStatus.err, false, false });
        }
        mState.condition.notify_all();
    }

    void OnCommissioningStageStart(PeerId, chip::Controller::CommissioningStage stageStarting) override
    {
        CommissioningProgressCallback callback;
        {
            std::scoped_lock lock(mState.mutex);
            mState.activeStage = MakeOptional(stageStarting);
            callback           = mState.progressCallback;
        }
        if (callback)
        {
            callback({ stageStarting, CHIP_NO_ERROR, false, false });
        }
    }

    void OnCommissioningStatusUpdate(PeerId, chip::Controller::CommissioningStage stageCompleted, CHIP_ERROR error) override
    {
        CommissioningProgressCallback callback;
        {
            std::scoped_lock lock(mState.mutex);
            if (error == CHIP_NO_ERROR)
            {
                mState.lastCompletedStage = MakeOptional(stageCompleted);
                mState.activeStage        = NullOptional;
            }
            callback                  = mState.progressCallback;
        }
        if (callback)
        {
            callback({ stageCompleted, error, true, false });
        }
    }

    void OnCommissioningRetry(PeerId, chip::Controller::CommissioningStage stage, CHIP_ERROR error) override
    {
        CommissioningProgressCallback callback;
        {
            std::scoped_lock lock(mState.mutex);
            callback = mState.progressCallback;
        }
        if (callback)
        {
            callback({ stage, error, false, true });
        }
    }

    Optional<uint16_t> FailSafeExpiryTimeoutSecs() const override { return NullOptional; }

    void OnDeviceAttestationCompleted(DeviceCommissioner * commissioner, DeviceProxy * device,
                                      const Credentials::DeviceAttestationVerifier::AttestationDeviceInfo &,
                                      Credentials::AttestationVerificationResult result) override
    {
        if (result != Credentials::AttestationVerificationResult::kSuccess && !mAllowTestAttestation)
        {
            Complete(CHIP_ERROR_CERT_NOT_TRUSTED);
            return;
        }

        CHIP_ERROR error =
            commissioner->ContinueCommissioningAfterDeviceAttestation(device, Credentials::AttestationVerificationResult::kSuccess);
        if (error != CHIP_NO_ERROR)
        {
            Complete(error);
        }
    }

private:
    void Complete(CHIP_ERROR error)
    {
        {
            std::scoped_lock lock(mState.mutex);
            mState.complete = true;
            mState.error    = error;
        }
        mState.condition.notify_all();
    }

    PairingState & mState;
    bool mAllowTestAttestation;
};

enum class OnOffOperation
{
    Read,
    On,
    Off,
    Toggle,
};

struct OnOffOperationState
{
    OnOffOperationState(OnOffOperation requestedOperation, EndpointId requestedEndpoint) :
        operation(requestedOperation), endpoint(requestedEndpoint), onConnected(&HandleConnected, this),
        onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void Finish(CHIP_ERROR result)
    {
        {
            std::scoped_lock lock(mutex);
            if (complete)
            {
                return;
            }
            error    = result;
            complete = true;
        }
        condition.notify_all();
    }

    void FinishRead(bool value)
    {
        valueRead = value;
        Finish(CHIP_NO_ERROR);
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<OnOffOperationState *>(context);
        CHIP_ERROR error;
        if (state->operation == OnOffOperation::Read)
        {
            ClusterBase cluster(exchangeManager, session, state->endpoint);
            error = cluster.ReadAttribute<app::Clusters::OnOff::Attributes::OnOff::TypeInfo>(
                state, [](void * callbackContext, bool value) { static_cast<OnOffOperationState *>(callbackContext)->FinishRead(value); },
                [](void * callbackContext, CHIP_ERROR readError) {
                    static_cast<OnOffOperationState *>(callbackContext)->Finish(readError);
                });
        }
        else
        {
            auto success = [state](app::ConcreteCommandPath const &, app::StatusIB const & status,
                                   app::DataModel::NullObjectType const &) { state->Finish(status.ToChipError()); };
            auto failure = [state](CHIP_ERROR commandError) { state->Finish(commandError); };
            if (state->operation == OnOffOperation::On)
            {
                app::Clusters::OnOff::Commands::On::Type request;
                error = InvokeCommandRequest(&exchangeManager, session, state->endpoint, request, success, failure);
            }
            else if (state->operation == OnOffOperation::Off)
            {
                app::Clusters::OnOff::Commands::Off::Type request;
                error = InvokeCommandRequest(&exchangeManager, session, state->endpoint, request, success, failure);
            }
            else
            {
                app::Clusters::OnOff::Commands::Toggle::Type request;
                error = InvokeCommandRequest(&exchangeManager, session, state->endpoint, request, success, failure);
            }
        }
        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<OnOffOperationState *>(context)->Finish(error);
    }

    OnOffOperation operation;
    EndpointId endpoint;
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    bool valueRead = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;
};

template <typename Request>
struct CommandOperationState
{
    CommandOperationState(EndpointId requestedEndpoint, Request requested) :
        endpoint(requestedEndpoint), request(std::move(requested)), onConnected(&HandleConnected, this),
        onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void Finish(CHIP_ERROR result)
    {
        {
            std::scoped_lock lock(mutex);
            if (complete)
            {
                return;
            }
            complete = true;
            error    = result;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<CommandOperationState *>(context);
        auto success = [state](app::ConcreteCommandPath const &, app::StatusIB const & status,
                               typename Request::ResponseType const &) { state->Finish(status.ToChipError()); };
        auto failure = [state](CHIP_ERROR error) { state->Finish(error); };
        CHIP_ERROR error = InvokeCommandRequest(&exchangeManager, session, state->endpoint, state->request, success, failure);
        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<CommandOperationState *>(context)->Finish(error);
    }

    EndpointId endpoint;
    Request request;
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;
};

template <typename AttributeType, typename Value>
struct ReadOperationState
{
    using DecodableType = typename AttributeType::DecodableType;
    using DecodableArgType = typename AttributeType::DecodableArgType;
    using Converter = std::function<Value(DecodableArgType)>;

    ReadOperationState(EndpointId requestedEndpoint, Converter valueConverter) :
        endpoint(requestedEndpoint), converter(std::move(valueConverter)), onConnected(&HandleConnected, this),
        onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void Finish(CHIP_ERROR result)
    {
        {
            std::scoped_lock lock(mutex);
            if (complete)
            {
                return;
            }
            complete = true;
            error    = result;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<ReadOperationState *>(context);
        ClusterBase cluster(exchangeManager, session, state->endpoint);
        CHIP_ERROR error = cluster.ReadAttribute<AttributeType>(
            state,
            [](void * callbackContext, DecodableArgType decoded) {
                auto * operation = static_cast<ReadOperationState *>(callbackContext);
                operation->value = operation->converter(decoded);
                operation->Finish(CHIP_NO_ERROR);
            },
            [](void * callbackContext, CHIP_ERROR readError) {
                static_cast<ReadOperationState *>(callbackContext)->Finish(readError);
            });
        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<ReadOperationState *>(context)->Finish(error);
    }

    EndpointId endpoint;
    Converter converter;
    Value value{};
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;
};

class GenericReadState final : public app::ReadClient::Callback
{
public:
    using ReportCallback =
        std::function<void(app::ConcreteDataAttributePath const &, Windows::Foundation::Collections::IPropertySet const &)>;

    GenericReadState(EndpointId endpoint, ClusterId cluster, AttributeId attribute, bool subscription,
                     uint16_t minimumInterval, uint16_t maximumInterval, ReportCallback reportCallback = {}) :
        mPath(endpoint, cluster, attribute), mSubscription(subscription), mMinimumInterval(minimumInterval),
        mMaximumInterval(maximumInterval), mReportCallback(std::move(reportCallback)),
        onConnected(&HandleConnected, this), onConnectionFailure(&HandleConnectionFailure, this),
        mBufferedCallback(*this, true)
    {}

    void OnAttributeData(app::ConcreteDataAttributePath const & path, TLV::TLVReader * data,
                         app::StatusIB const & status) override
    {
        CHIP_ERROR error = status.ToChipError();
        if (error != CHIP_NO_ERROR || data == nullptr)
        {
            Fail(error != CHIP_NO_ERROR ? error : CHIP_ERROR_INVALID_ARGUMENT);
            return;
        }
        try
        {
            auto decoded = DecodePropertySetRoot(*data);
            {
                std::scoped_lock lock(mutex);
                value      = decoded;
                valueReady = true;
            }
            if (mReportCallback)
            {
                mReportCallback(path, decoded);
            }
        }
        catch (hresult_error const &)
        {
            Fail(CHIP_ERROR_DECODE_FAILED);
        }
        condition.notify_all();
    }

    void OnError(CHIP_ERROR error) override { Fail(error); }

    void OnDone(app::ReadClient *) override
    {
        if (!mSubscription)
        {
            std::scoped_lock lock(mutex);
            complete = true;
            condition.notify_all();
        }
    }

    void OnSubscriptionEstablished(SubscriptionId) override
    {
        {
            std::scoped_lock lock(mutex);
            established = true;
        }
        condition.notify_all();
    }

    void Fail(CHIP_ERROR error)
    {
        {
            std::scoped_lock lock(mutex);
            if (closed)
            {
                return;
            }
            result   = error;
            complete = true;
        }
        condition.notify_all();
    }

    void Close()
    {
        {
            std::scoped_lock lock(mutex);
            if (closed)
            {
                return;
            }
        }
        PlatformMgr().LockChipStack();
        onConnected.Cancel();
        onConnectionFailure.Cancel();
        client.reset();
        PlatformMgr().UnlockChipStack();
        {
            std::scoped_lock lock(mutex);
            closed   = true;
            complete = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<GenericReadState *>(context);
        state->client = Platform::MakeUnique<app::ReadClient>(
            app::InteractionModelEngine::GetInstance(), &exchangeManager, state->mBufferedCallback,
            state->mSubscription ? app::ReadClient::InteractionType::Subscribe : app::ReadClient::InteractionType::Read);
        if (!state->client)
        {
            state->Fail(CHIP_ERROR_NO_MEMORY);
            return;
        }
        app::ReadPrepareParams parameters(session);
        parameters.mpAttributePathParamsList     = &state->mPath;
        parameters.mAttributePathParamsListSize  = 1;
        parameters.mIsFabricFiltered             = true;
        parameters.mMinIntervalFloorSeconds      = state->mMinimumInterval;
        parameters.mMaxIntervalCeilingSeconds    = state->mMaximumInterval;
        CHIP_ERROR error = state->client->SendRequest(parameters);
        if (error != CHIP_NO_ERROR)
        {
            state->Fail(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<GenericReadState *>(context)->Fail(error);
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    bool established = false;
    bool valueReady = false;
    bool closed = false;
    CHIP_ERROR result = CHIP_NO_ERROR;
    Windows::Foundation::Collections::IPropertySet value{ nullptr };
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;

private:
    app::AttributePathParams mPath;
    bool mSubscription;
    uint16_t mMinimumInterval;
    uint16_t mMaximumInterval;
    ReportCallback mReportCallback;
    app::BufferedReadCallback mBufferedCallback;
    Platform::UniquePtr<app::ReadClient> client;
};

class GenericEventReadState final : public app::ReadClient::Callback
{
public:
    using ReportCallback = std::function<void(Controller::EventValue const &)>;

    GenericEventReadState(EndpointId endpoint, ClusterId cluster, EventId event, bool urgent, EventNumber minimumEventNumber,
                          bool subscription, uint16_t minimumInterval, uint16_t maximumInterval,
                          ReportCallback reportCallback = {}) :
        mPath(endpoint, cluster, event, urgent), mMinimumEventNumber(minimumEventNumber), mSubscription(subscription),
        mMinimumInterval(minimumInterval), mMaximumInterval(maximumInterval), mReportCallback(std::move(reportCallback)),
        onConnected(&HandleConnected, this), onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void OnEventData(app::EventHeader const & header, TLV::TLVReader * data, app::StatusIB const * status) override
    {
        CHIP_ERROR error = status == nullptr ? CHIP_NO_ERROR : status->ToChipError();
        if (error != CHIP_NO_ERROR || data == nullptr)
        {
            Fail(error != CHIP_NO_ERROR ? error : CHIP_ERROR_INVALID_ARGUMENT);
            return;
        }
        try
        {
            auto decoded = DecodePropertySetRoot(*data);
            auto path = winrt::make<implementation::EventPath>(header.mPath.mEndpointId, header.mPath.mClusterId,
                                                               header.mPath.mEventId, mPath.mIsUrgentEvent);
            auto value = winrt::make<implementation::EventValue>(path, header.mEventNumber, decoded);
            if (!mSubscription)
            {
                std::scoped_lock lock(mutex);
                values.push_back(value);
            }
            if (mReportCallback)
            {
                mReportCallback(value);
            }
        }
        catch (hresult_error const &)
        {
            Fail(CHIP_ERROR_DECODE_FAILED);
        }
        condition.notify_all();
    }

    void OnError(CHIP_ERROR error) override { Fail(error); }

    void OnDone(app::ReadClient *) override
    {
        if (!mSubscription)
        {
            std::scoped_lock lock(mutex);
            complete = true;
            condition.notify_all();
        }
    }

    void OnSubscriptionEstablished(SubscriptionId) override
    {
        {
            std::scoped_lock lock(mutex);
            established = true;
        }
        condition.notify_all();
    }

    void Fail(CHIP_ERROR error)
    {
        {
            std::scoped_lock lock(mutex);
            if (closed)
            {
                return;
            }
            result   = error;
            complete = true;
        }
        condition.notify_all();
    }

    void Close()
    {
        {
            std::scoped_lock lock(mutex);
            if (closed)
            {
                return;
            }
        }
        PlatformMgr().LockChipStack();
        onConnected.Cancel();
        onConnectionFailure.Cancel();
        client.reset();
        PlatformMgr().UnlockChipStack();
        {
            std::scoped_lock lock(mutex);
            closed   = true;
            complete = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<GenericEventReadState *>(context);
        state->client = Platform::MakeUnique<app::ReadClient>(
            app::InteractionModelEngine::GetInstance(), &exchangeManager, *state,
            state->mSubscription ? app::ReadClient::InteractionType::Subscribe : app::ReadClient::InteractionType::Read);
        if (!state->client)
        {
            state->Fail(CHIP_ERROR_NO_MEMORY);
            return;
        }
        app::ReadPrepareParams parameters(session);
        parameters.mpEventPathParamsList       = &state->mPath;
        parameters.mEventPathParamsListSize    = 1;
        parameters.mEventNumber.SetValue(state->mMinimumEventNumber);
        parameters.mIsFabricFiltered           = true;
        parameters.mMinIntervalFloorSeconds    = state->mMinimumInterval;
        parameters.mMaxIntervalCeilingSeconds  = state->mMaximumInterval;
        CHIP_ERROR error = state->client->SendRequest(parameters);
        if (error != CHIP_NO_ERROR)
        {
            state->Fail(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<GenericEventReadState *>(context)->Fail(error);
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    bool established = false;
    bool closed = false;
    CHIP_ERROR result = CHIP_NO_ERROR;
    std::vector<Controller::EventValue> values;
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;

private:
    app::EventPathParams mPath;
    EventNumber mMinimumEventNumber;
    bool mSubscription;
    uint16_t mMinimumInterval;
    uint16_t mMaximumInterval;
    ReportCallback mReportCallback;
    Platform::UniquePtr<app::ReadClient> client;
};

class GenericWriteState final : public app::WriteClient::Callback
{
public:
    GenericWriteState(EndpointId endpoint, ClusterId cluster, AttributeId attribute, std::vector<uint8_t> encoded,
                      std::optional<uint16_t> timedWriteTimeout) :
        mPath(endpoint, cluster, attribute), mEncoded(std::move(encoded)), mTimedWriteTimeout(timedWriteTimeout),
        onConnected(&HandleConnected, this), onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void OnResponse(app::WriteClient const *, app::ConcreteDataAttributePath const &, app::StatusIB status) override
    {
        CHIP_ERROR error = status.ToChipError();
        if (error != CHIP_NO_ERROR)
        {
            result = error;
        }
    }

    void OnError(app::WriteClient const *, CHIP_ERROR error) override { result = error; }

    void OnDone(app::WriteClient *) override
    {
        {
            std::scoped_lock lock(mutex);
            complete = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<GenericWriteState *>(context);
        Optional<uint16_t> timedWriteTimeout;
        if (state->mTimedWriteTimeout.has_value())
        {
            timedWriteTimeout.SetValue(*state->mTimedWriteTimeout);
        }
        state->client = Platform::MakeUnique<app::WriteClient>(&exchangeManager, state, timedWriteTimeout);
        if (!state->client)
        {
            state->Finish(CHIP_ERROR_NO_MEMORY);
            return;
        }
        TLV::TLVReader reader;
        reader.Init(state->mEncoded.data(), state->mEncoded.size());
        CHIP_ERROR error = reader.Next();
        if (error == CHIP_NO_ERROR)
        {
            error = state->client->PutPreencodedAttribute(state->mPath, reader);
        }
        if (error == CHIP_NO_ERROR)
        {
            error = state->client->SendWriteRequest(session);
        }
        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<GenericWriteState *>(context)->Finish(error);
    }

    void Finish(CHIP_ERROR error)
    {
        {
            std::scoped_lock lock(mutex);
            result   = error;
            complete = true;
        }
        condition.notify_all();
    }

    void ReleaseClient()
    {
        PlatformMgr().LockChipStack();
        onConnected.Cancel();
        onConnectionFailure.Cancel();
        client.reset();
        PlatformMgr().UnlockChipStack();
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    CHIP_ERROR result = CHIP_NO_ERROR;
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;

private:
    app::ConcreteDataAttributePath mPath;
    std::vector<uint8_t> mEncoded;
    std::optional<uint16_t> mTimedWriteTimeout;
    Platform::UniquePtr<app::WriteClient> client;
};

class GenericInvokeState final : public app::CommandSender::Callback
{
public:
    GenericInvokeState(EndpointId endpoint, ClusterId cluster, CommandId command, std::vector<uint8_t> encoded,
                       std::optional<uint16_t> timedInvokeTimeout, std::optional<CommandId> responseCommand = std::nullopt,
                       bool responseData = false) :
        mPath(endpoint, cluster, command, app::CommandPathFlags::kEndpointIdValid), mEncoded(std::move(encoded)),
        mTimedInvokeTimeout(timedInvokeTimeout), mResponseCommand(responseCommand), mResponseData(responseData),
        onConnected(&HandleConnected, this),
        onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void OnResponse(app::CommandSender *, app::ConcreteCommandPath const & path, app::StatusIB const & status,
                    TLV::TLVReader * data) override
    {
        if (mResponseCommand && (mResponseSeen || path.mEndpointId != mPath.mEndpointId ||
                                path.mClusterId != mPath.mClusterId ||
                                (status.IsSuccess() && (path.mCommandId != *mResponseCommand ||
                                                       (mResponseData && data == nullptr)))))
        {
            result = CHIP_ERROR_INVALID_ARGUMENT;
            return;
        }
        mResponseSeen = true;
        CHIP_ERROR error = status.ToChipError();
        if (error != CHIP_NO_ERROR)
        {
            result = error;
            return;
        }
        if (data != nullptr)
        {
            try
            {
                value = DecodePropertySetRoot(*data);
            }
            catch (hresult_error const &)
            {
                result = CHIP_ERROR_DECODE_FAILED;
            }
        }
        else
        {
            value = Windows::Foundation::Collections::PropertySet();
        }
    }

    void OnError(app::CommandSender const *, CHIP_ERROR error) override { result = error; }

    void OnDone(app::CommandSender *) override
    {
        {
            std::scoped_lock lock(mutex);
            if (mResponseCommand && !mResponseSeen && result == CHIP_NO_ERROR)
            {
                result = CHIP_ERROR_TIMEOUT;
            }
            complete = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<GenericInvokeState *>(context);
        state->sender =
            Platform::MakeUnique<app::CommandSender>(state, &exchangeManager, state->mTimedInvokeTimeout.has_value());
        if (!state->sender)
        {
            state->Finish(CHIP_ERROR_NO_MEMORY);
            return;
        }
        app::CommandSender::PrepareCommandParameters prepareParameters;
        CHIP_ERROR error = state->sender->PrepareCommand(state->mPath, prepareParameters);
        TLV::TLVWriter * writer = state->sender->GetCommandDataIBTLVWriter();
        if (error == CHIP_NO_ERROR && writer == nullptr)
        {
            error = CHIP_ERROR_INCORRECT_STATE;
        }
        if (error == CHIP_NO_ERROR)
        {
            TLV::TLVReader reader;
            reader.Init(state->mEncoded.data(), state->mEncoded.size());
            error = reader.Next();
            if (error == CHIP_NO_ERROR)
            {
                error = writer->CopyContainer(TLV::ContextTag(app::CommandDataIB::Tag::kFields), reader);
            }
        }
        if (error == CHIP_NO_ERROR)
        {
            Optional<uint16_t> timedInvokeTimeout;
            if (state->mTimedInvokeTimeout.has_value())
            {
                timedInvokeTimeout.SetValue(*state->mTimedInvokeTimeout);
            }
            app::CommandSender::FinishCommandParameters finishParameters(timedInvokeTimeout);
            error = state->sender->FinishCommand(finishParameters);
        }
        if (error == CHIP_NO_ERROR)
        {
            error = state->sender->SendCommandRequest(session);
        }
        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<GenericInvokeState *>(context)->Finish(error);
    }

    void Finish(CHIP_ERROR error)
    {
        {
            std::scoped_lock lock(mutex);
            result   = error;
            complete = true;
        }
        condition.notify_all();
    }

    void ReleaseSender()
    {
        PlatformMgr().LockChipStack();
        onConnected.Cancel();
        onConnectionFailure.Cancel();
        sender.reset();
        PlatformMgr().UnlockChipStack();
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    CHIP_ERROR result = CHIP_NO_ERROR;
    Windows::Foundation::Collections::IPropertySet value{ nullptr };
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;

private:
    app::CommandPathParams mPath;
    std::vector<uint8_t> mEncoded;
    std::optional<uint16_t> mTimedInvokeTimeout;
    std::optional<CommandId> mResponseCommand;
    bool mResponseData = false;
    bool mResponseSeen = false;
    Platform::UniquePtr<app::CommandSender> sender;
};

std::string GenerateSetupCode(uint32_t pinCode, uint16_t discriminator)
{
    SetupPayload payload;
    payload.setUpPINCode = pinCode;
    payload.discriminator.SetLongValue(discriminator);
    std::string setupCode;
    CheckChipError(ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(setupCode),
                   L"Generate manual setup code");
    return setupCode;
}

} // namespace

class ControllerRuntime : public std::enable_shared_from_this<ControllerRuntime>
{
#if defined(MATTER_WINRT_ADMISSION_TESTS)
    friend struct RuntimeAdmissionTests;
#endif
public:
    static std::shared_ptr<ControllerRuntime> Create(Controller::ControllerOptions const & options)
    {
        if (options == nullptr || options.StoragePath().empty())
        {
            throw hresult_invalid_argument(L"ControllerOptions.StoragePath is required.");
        }

        std::scoped_lock lock(sMutex);
        if (auto previous = sInstance.lock())
        {
            std::scoped_lock previousOperationLock(previous->mOperationMutex);
            if (!previous->mClosed)
            {
                throw hresult_illegal_method_call(L"Only one MatterController may be active in a process.");
            }
        }

        auto runtime = std::shared_ptr<ControllerRuntime>(new ControllerRuntime(options));
        runtime->Initialize();
        sInstance = runtime;
        return runtime;
    }

    void Close()
    {
        auto lifetime = mAdmission.BeginShutdown();
        std::scoped_lock operationLock(mOperationMutex);
        std::scoped_lock lock(mMutex);
        if (mClosed)
        {
            return;
        }

        for (auto const & subscription : mSubscriptions)
        {
            subscription->Close();
        }
        mSubscriptions.clear();
        for (auto const & subscription : mEventSubscriptions)
        {
            subscription->Close();
        }
        mEventSubscriptions.clear();
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        if (mCameraHost && mCameraHost->ActiveHandle() != 0 && mCommissionerInitialized)
        {
            LogErrorOnFailure(mCameraHost->Send(mCommissioner, mCameraHost->ActiveHandle(),
                                               CameraControllerRuntime::Client::Command::End, {}, {}, {}));
        }
#endif
        (void) PlatformMgr().StopEventLoopTask();
        PlatformMgr().LockChipStack();
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        if (mCameraHost)
        {
            LogErrorOnFailure(mCameraHost->Shutdown());
        }
#endif
        if (mCommissionerInitialized)
        {
            mCommissioner.Shutdown();
            mCommissionerInitialized = false;
        }
        if (mFactoryInitialized)
        {
            DeviceControllerFactory::GetInstance().Shutdown();
            mFactoryInitialized = false;
        }
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        mCameraHost.reset();
#endif
        Credentials::SetGroupDataProvider(nullptr);
        mGroupDataProvider.Finish();
        mOpCertStore.Finish();
        mOperationalKeystore.Finish();
        PlatformMgr().UnlockChipStack();
        Platform::MemoryShutdown();
        mClosed = true;
    }

    uint16_t FabricIndex()
    {
        auto lifetime = mAdmission.AcquireLifetime();
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        return mCommissioner.GetFabricIndex();
    }

    Controller::CommissionedNode Commission(uint64_t nodeId, uint32_t pinCode, uint16_t discriminator, bool useBle,
                                            std::string const & providedSetupCode = {},
                                            std::optional<WiFiCredentials> const & wiFiCredentials = std::nullopt,
                                            ByteSpan threadOperationalDataset = {},
                                            Optional<AddressResolve::InterfaceSelection> interfaceSelection = NullOptional,
                                            CommissioningProgressCallback progressCallback = {},
                                            std::shared_ptr<std::atomic_bool> cancellationRequested = {})
    {
        auto lifetime = mAdmission.AcquireExclusive(cancellationRequested);
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        if (cancellationRequested && cancellationRequested->load(std::memory_order_acquire))
        {
            throw hresult_canceled();
        }
        mPairingDelegate.Reset(std::move(progressCallback));
        std::string setupCode = providedSetupCode.empty() ? GenerateSetupCode(pinCode, discriminator) : providedSetupCode;

        CommissioningParameters parameters;
        parameters.SetDeviceAttestationDelegate(&mPairingDelegate);
        if (wiFiCredentials.has_value())
        {
            parameters.SetWiFiCredentials(*wiFiCredentials);
        }
        if (!threadOperationalDataset.empty())
        {
            parameters.SetThreadOperationalDataset(threadOperationalDataset);
        }
        if (interfaceSelection.HasValue())
        {
            parameters.SetOperationalInterfaceSelection(interfaceSelection.Value());
        }
        PlatformMgr().LockChipStack();
        CHIP_ERROR error = mCommissioner.PairDevice(
            nodeId, setupCode.c_str(), parameters, useBle ? DiscoveryType::kDiscoveryBleOnly : DiscoveryType::kDiscoveryNetworkOnly);
        PlatformMgr().UnlockChipStack();
        CheckChipError(error, L"Start commissioning");
        if (cancellationRequested && cancellationRequested->load(std::memory_order_acquire))
        {
            CancelCommission(nodeId);
        }

        std::unique_lock lock(mPairingState.mutex);
        if (!mPairingState.condition.wait_for(lock, kCommissioningTimeout, [this, &cancellationRequested]() {
                return mPairingState.complete ||
                    (cancellationRequested && cancellationRequested->load(std::memory_order_acquire));
            }))
        {
            std::wstring timeoutMessage = CommissioningTimeoutMessage(mPairingState);
            lock.unlock();
            PlatformMgr().LockChipStack();
            (void) mCommissioner.StopPairing(nodeId);
            PlatformMgr().UnlockChipStack();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), timeoutMessage);
        }
        if (cancellationRequested && cancellationRequested->load(std::memory_order_acquire))
        {
            lock.unlock();
            CancelCommission(nodeId);
            throw hresult_canceled();
        }
        if (mPairingState.error != CHIP_NO_ERROR && !mPairingState.hasCompletionStatus)
        {
            (void) mPairingState.condition.wait_for(lock, kCommissioningFailureDetailTimeout,
                                                    [this]() { return mPairingState.hasCompletionStatus; });
        }
        if (mPairingState.error != CHIP_NO_ERROR && mPairingState.hasCompletionStatus)
        {
            ThrowCommissioningError(mPairingState);
        }
        CheckChipError(mPairingState.error, L"Commission device");
        if (std::find(mCommissionedNodes.begin(), mCommissionedNodes.end(), nodeId) == mCommissionedNodes.end())
        {
            mCommissionedNodes.push_back(nodeId);
            PersistCommissionedNodes();
        }
        return winrt::make<implementation::CommissionedNode>(nodeId, mCommissioner.GetFabricIndex());
    }

    void CancelCommission(uint64_t nodeId)
    {
        PlatformMgr().LockChipStack();
        (void) mCommissioner.StopPairing(nodeId);
        PlatformMgr().UnlockChipStack();
    }

    void NotifyCommissionCancellation()
    {
        std::scoped_lock lock(mPairingState.mutex);
        mPairingState.condition.notify_all();
    }

    std::vector<uint64_t> CommissionedNodes()
    {
        auto lifetime = mAdmission.AcquireLifetime();
        std::scoped_lock lock(mOperationMutex);
        EnsureOpen();
        return mCommissionedNodes;
    }

    Controller::CommissionedNode RecoverNode(uint64_t nodeId)
    {
        if (!IsOperationalNodeId(nodeId))
        {
            throw hresult_invalid_argument(L"nodeId must be an operational Matter node identifier.");
        }

        (void) ReadAttribute(nodeId, 0, app::Clusters::BasicInformation::Id,
                             app::Clusters::BasicInformation::Attributes::VendorID::Id);

        auto lifetime = mAdmission.AcquireExclusive();
        std::scoped_lock lock(mOperationMutex);
        EnsureOpen();
        if (std::find(mCommissionedNodes.begin(), mCommissionedNodes.end(), nodeId) == mCommissionedNodes.end())
        {
            mCommissionedNodes.push_back(nodeId);
            PersistCommissionedNodes();
        }
        return winrt::make<implementation::CommissionedNode>(nodeId, mCommissioner.GetFabricIndex());
    }

    void RemoveNode(uint64_t nodeId)
    {
        auto lifetime = mAdmission.AcquireExclusive();
        std::scoped_lock lock(mOperationMutex);
        EnsureOpen();
        PlatformMgr().LockChipStack();
        CHIP_ERROR error = mCommissioner.UnpairDevice(nodeId);
        PlatformMgr().UnlockChipStack();
        CheckChipError(error, L"Start removing Matter node");
        mCommissionedNodes.erase(std::remove(mCommissionedNodes.begin(), mCommissionedNodes.end(), nodeId),
                                 mCommissionedNodes.end());
        PersistCommissionedNodes();
    }

    bool RunOnOff(uint64_t nodeId, uint16_t endpointId, OnOffOperation operation)
    {
        if (operation == OnOffOperation::Read)
        {
            return PropertySetScalar<bool>(
                ReadAttribute(nodeId, endpointId, app::Clusters::OnOff::Id, app::Clusters::OnOff::Attributes::OnOff::Id),
                L"OnOff");
        }

        CommandId commandId;
        switch (operation)
        {
        case OnOffOperation::On:
            commandId = app::Clusters::OnOff::Commands::On::Id;
            break;
        case OnOffOperation::Off:
            commandId = app::Clusters::OnOff::Commands::Off::Id;
            break;
        case OnOffOperation::Toggle:
            commandId = app::Clusters::OnOff::Commands::Toggle::Id;
            break;
        default:
            throw hresult_invalid_argument(L"Unsupported On/Off operation.");
        }
        InvokeCommand(nodeId, endpointId, app::Clusters::OnOff::Id, commandId,
                      Windows::Foundation::Collections::PropertySet());
        return false;
    }

    template <typename Request>
    void Invoke(uint64_t nodeId, uint16_t endpointId, Request request)
    {
        auto operation = mAdmission.AcquireNode(nodeId);
        EnsureOpen();
        CommandOperationState<Request> state(endpointId, std::move(request));
        PlatformMgr().LockChipStack();
        CHIP_ERROR error = mCommissioner.GetConnectedDevice(nodeId, &state.onConnected, &state.onConnectionFailure);
        PlatformMgr().UnlockChipStack();
        CheckChipError(error, L"Start CASE session");

        std::unique_lock lock(state.mutex);
        if (!state.condition.wait_for(lock, kInteractionTimeout, [&state]() { return state.complete; }))
        {
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter command timed out.");
        }
        CheckChipError(state.error, L"Invoke Matter command");
    }

    template <typename AttributeType, typename Value>
    Value Read(uint64_t nodeId, uint16_t endpointId,
               typename ReadOperationState<AttributeType, Value>::Converter converter)
    {
        auto operation = mAdmission.AcquireNode(nodeId);
        EnsureOpen();
        ReadOperationState<AttributeType, Value> state(endpointId, std::move(converter));
        PlatformMgr().LockChipStack();
        CHIP_ERROR error = mCommissioner.GetConnectedDevice(nodeId, &state.onConnected, &state.onConnectionFailure);
        PlatformMgr().UnlockChipStack();
        CheckChipError(error, L"Start CASE session");

        std::unique_lock lock(state.mutex);
        if (!state.condition.wait_for(lock, kInteractionTimeout, [&state]() { return state.complete; }))
        {
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter attribute read timed out.");
        }
        CheckChipError(state.error, L"Read Matter attribute");
        return std::move(state.value);
    }

    Windows::Foundation::Collections::IPropertySet ReadAttribute(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId,
                                                                  AttributeId attributeId,
                                                                  std::shared_ptr<std::atomic_bool> cancellation = {},
                                                                  std::chrono::steady_clock::time_point deadline =
                                                                      std::chrono::steady_clock::time_point::max())
    {
        deadline = std::min(deadline, std::chrono::steady_clock::now() + kInteractionTimeout);
        auto operation = mAdmission.AcquireNode(nodeId, cancellation, deadline);
        EnsureOpen();
        auto state = std::make_shared<GenericReadState>(endpointId, clusterId, attributeId, false, 0, 0);
        auto cleanup = chip::ScopeExit([&] { state->Close(); });
        if (cancellation && cancellation->load(std::memory_order_acquire))
        {
            throw hresult_canceled();
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter attribute read timed out.");
        }
        StartConnectedOperation(nodeId, state->onConnected, state->onConnectionFailure);
        std::unique_lock lock(state->mutex);
        while (!state->complete)
        {
            if (cancellation && cancellation->load(std::memory_order_acquire))
            {
                lock.unlock();
                throw hresult_canceled();
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                lock.unlock();
                throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter attribute read timed out.");
            }
            const auto wake = cancellation ? std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(100))
                                           : deadline;
            state->condition.wait_until(lock, wake, [&state]() { return state->complete; });
        }
        CHIP_ERROR result = state->result;
        auto value        = state->value;
        lock.unlock();
        CheckChipError(result, L"Read Matter attribute");
        if (!value)
        {
            throw hresult_error(E_FAIL, L"The Matter attribute read returned no value.");
        }
        return value;
    }

    Controller::MatterCameraSession StartCamera(uint64_t nodeId, uint16_t endpoint, uint16_t stream, std::string sdp,
        std::shared_ptr<std::atomic_bool> const & canceled,
        std::vector<Controller::MatterCameraIceServer> const & iceServers)
    {
        auto video = winrt::make<implementation::MatterCameraStreamSelection>(
            Controller::MatterCameraStreamSelectionKind::ExplicitId, box_value(stream).as<Windows::Foundation::IReference<uint16_t>>());
        auto audio = winrt::make<implementation::MatterCameraStreamSelection>(
            Controller::MatterCameraStreamSelectionKind::Absent, nullptr);
        return StartCamera(nodeId, endpoint, video, audio, std::move(sdp), canceled, iceServers);
    }

    CameraAudioAllocationOwnership & AudioOwnership() { return mAudioOwnership; }

    std::pair<std::optional<uint16_t>, std::optional<uint16_t>> SelectedCameraStreams(uint64_t handle)
    {
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        auto lifetime = mAdmission.AcquireLifetime();
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        CameraStackLock lock;
        CameraControllerRuntime::Cluster::SessionInfo info;
        CheckChipError(mCameraHost->GetSessionInfo(handle, info), L"Read selected camera streams");
        auto id = [](auto const & field) -> std::optional<uint16_t> {
            return field.IsNull() ? std::nullopt : std::optional<uint16_t>(field.Value());
        };
        return { id(info.metadata.videoStreamID), id(info.metadata.audioStreamID) };
#else
        throw hresult_not_implemented(L"Camera signaling is not compiled into this component.");
#endif
    }

    Controller::MatterCameraSession StartCamera(uint64_t nodeId, uint16_t endpoint,
        Controller::MatterCameraStreamSelection const & video, Controller::MatterCameraStreamSelection const & audio,
        std::string sdp, std::shared_ptr<std::atomic_bool> const & canceled,
        std::vector<Controller::MatterCameraIceServer> const & iceServers)
    {
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        const auto deadline = std::chrono::steady_clock::now() + kInteractionTimeout;
        auto operation = mAdmission.AcquireNode(nodeId, canceled, deadline);
        auto cameraLock = mAdmission.AcquireGate(mCameraOperationMutex, canceled, deadline);
        EnsureOpen();
        if (!mCameraHost)
        {
            throw hresult_illegal_method_call(L"EnableCameraSignaling must be set before creating the controller.");
        }
        if (canceled && canceled->load(std::memory_order_acquire))
        {
            throw hresult_canceled();
        }
        std::vector<CameraControllerRuntime::IceServerConfiguration> servers;
        for (auto const & source : iceServers)
        {
            CameraControllerRuntime::IceServerConfiguration server;
            for (auto const & url : source.Urls())
            {
                server.urls.push_back(to_string(url));
            }
            if (source.HasUsername())
            {
                server.username = to_string(source.Username());
            }
            if (source.HasCredential())
            {
                server.credential = to_string(source.Credential());
            }
            if (source.CertificateAuthorityId())
            {
                server.certificateAuthorityId = source.CertificateAuthorityId().Value();
            }
            servers.push_back(std::move(server));
        }
        uint64_t handle = 0;
        uint16_t requestor = kInvalidEndpointId;
        {
            CameraStackLock lock;
            auto * systemState = DeviceControllerFactory::GetInstance().GetSystemState();
            if (!systemState || !systemState->Fabrics() || !systemState->CASESessionMgr() || !systemState->SystemLayer())
            {
                throw hresult_illegal_method_call(L"The native controller system state is unavailable.");
            }
            auto selection = [](Controller::MatterCameraStreamSelection const & source) {
                using Selection = CameraControllerRuntime::Cluster::StreamSelection;
                switch (source.Kind())
                {
                case Controller::MatterCameraStreamSelectionKind::Absent: return Selection::Absent();
                case Controller::MatterCameraStreamSelectionKind::AutoSelect: return Selection::AutoSelect();
                case Controller::MatterCameraStreamSelectionKind::ExplicitId: return Selection::ExplicitId(source.StreamId().Value());
                default: throw hresult_invalid_argument(L"Unknown camera media selection.");
                }
            };
            CheckChipError(mCameraHost->Create(*systemState->Fabrics(), *systemState->CASESessionMgr(), *systemState->SystemLayer(),
                ScopedNodeId(nodeId, mCommissioner.GetFabricIndex()), endpoint, selection(video), selection(audio), handle),
                L"Create camera session");
            requestor = mCameraHost->RequestorEndpoint();
        }
        CHIP_ERROR error = mCameraHost->Send(mCommissioner, handle, CameraControllerRuntime::Client::Command::Offer,
                                             std::move(sdp), {}, canceled, std::move(servers));
        if (error == CHIP_NO_ERROR && canceled && canceled->load(std::memory_order_acquire))
        {
            error = CHIP_ERROR_CANCELLED;
        }
        if (error != CHIP_NO_ERROR)
        {
            LogErrorOnFailure(mCameraHost->Send(mCommissioner, handle, CameraControllerRuntime::Client::Command::End,
                                               {}, {}, {}));
            CameraStackLock lock;
            LogErrorOnFailure(mCameraHost->CloseLocal(handle));
            if (error == CHIP_ERROR_CANCELLED)
            {
                throw hresult_canceled();
            }
            ThrowChipError(error, L"Send camera offer");
        }
        try
        {
            CameraStackLock lock;
            CameraControllerRuntime::Cluster::SessionInfo info;
            CheckChipError(mCameraHost->GetSessionInfo(handle, info), L"Read camera session metadata");
            auto id = [](auto const & value) -> std::optional<uint16_t> {
                return value.IsNull() ? std::nullopt : std::optional<uint16_t>(value.Value());
            };
            auto session = winrt::make_self<implementation::MatterCameraSession>(shared_from_this(), handle, nodeId, endpoint, requestor);
            session->SetMediaMetadata(video, audio, id(info.metadata.videoStreamID), id(info.metadata.audioStreamID), info.protocolId);
            return *session;
        }
        catch (...)
        {
            LogErrorOnFailure(mCameraHost->Send(mCommissioner, handle, CameraControllerRuntime::Client::Command::End, {}, {}, {}));
            CameraStackLock lock;
            LogErrorOnFailure(mCameraHost->CloseLocal(handle));
            throw;
        }
#else
        throw hresult_not_implemented(L"Camera signaling is not compiled into this component.");
#endif
    }

    void EnsureCameraSignalingEnabled()
    {
        auto lifetime = mAdmission.AcquireLifetime();
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        if (!mCameraHost)
        {
            throw hresult_illegal_method_call(L"EnableCameraSignaling must be set before creating the controller.");
        }
#else
        ThrowChipError(CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE, L"Camera signaling is not compiled into this component");
#endif
    }

    void SendCameraCandidates(uint64_t handle, std::vector<Controller::MatterCameraIceCandidate> const & candidates,
                              std::shared_ptr<std::atomic_bool> const & canceled)
    {
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        const auto deadline = std::chrono::steady_clock::now() + kInteractionTimeout;
        auto lifetime = mAdmission.AcquireLifetime(canceled, deadline);
        auto operation = mAdmission.AcquireNodeWithLifetime(
            std::move(lifetime), CameraPeerNode(handle), canceled, deadline);
        auto cameraLock = mAdmission.AcquireGate(mCameraOperationMutex, canceled, deadline);
        EnsureOpen();
        if (!mCameraHost)
        {
            throw hresult_illegal_method_call(L"Camera signaling is disabled.");
        }
        std::vector<CameraControllerRuntime::Cluster::Candidate> values;
        for (auto const & candidate : candidates)
        {
            CameraControllerRuntime::Cluster::Candidate value;
            value.candidate = to_string(candidate.Candidate());
            if (!candidate.Mid().empty())
            {
                value.mid = to_string(candidate.Mid());
            }
            if (candidate.LineIndex() >= 0)
            {
                value.lineIndex = static_cast<uint16_t>(candidate.LineIndex());
            }
            values.push_back(std::move(value));
        }
        const auto error = mCameraHost->Send(mCommissioner, handle, CameraControllerRuntime::Client::Command::Candidates,
                                             {}, std::move(values), canceled);
        if (error == CHIP_ERROR_CANCELLED)
        {
            throw hresult_canceled();
        }
        CheckChipError(error, L"Send camera ICE candidates");
#else
        throw hresult_not_implemented(L"Camera signaling is not compiled into this component.");
#endif
    }

    Controller::MatterCameraSignal PollCameraSignal(uint64_t handle)
    {
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        auto lifetime = mAdmission.AcquireLifetime();
        std::unique_lock operationLock(mOperationMutex, std::try_to_lock);
        if (!operationLock.owns_lock())
        {
            return nullptr;
        }
        EnsureOpen();
        if (!mCameraHost)
        {
            throw hresult_illegal_method_call(L"Camera signaling is disabled.");
        }
        CameraStackLock lock;
        CheckChipError(mCameraHost->Validate(handle), L"Validate camera session");
        auto signal = mCameraHost->TakeSignal(handle);
        if (!signal)
        {
            return nullptr;
        }
        std::vector<Controller::MatterCameraIceCandidate> candidates;
        for (auto const & candidate : signal->candidates)
        {
            candidates.push_back(winrt::make<implementation::MatterCameraIceCandidate>(
                to_hstring(candidate.candidate), candidate.mid ? to_hstring(*candidate.mid) : hstring{},
                candidate.lineIndex ? int32_t(*candidate.lineIndex) : -1));
        }
        return winrt::make<implementation::MatterCameraSignal>(handle,
            static_cast<Controller::MatterCameraSignalKind>(signal->type), to_hstring(signal->sdp),
            std::move(candidates), signal->error.AsInteger(), signal->protocolId, static_cast<uint8_t>(signal->reason));
#else
        throw hresult_not_implemented(L"Camera signaling is not compiled into this component.");
#endif
    }

    Controller::MatterCameraSessionState CameraState(uint64_t handle)
    {
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        auto lifetime = mAdmission.AcquireCleanup();
        if (!lifetime.owns_lock())
        {
            return Controller::MatterCameraSessionState::Closed;
        }
        std::scoped_lock operationLock(mOperationMutex);
        using State = Controller::MatterCameraSessionState;
        CameraStackLock lock;
        if (mClosed || !mCameraHost || mCameraHost->ActiveHandle() != handle)
        {
            return State::Closed;
        }
        CameraControllerRuntime::Cluster::SessionState state;
        CheckChipError(mCameraHost->GetSessionState(handle, state), L"Read camera signaling state");
        switch (state)
        {
        case CameraControllerRuntime::Cluster::SessionState::Negotiating: return State::Negotiating;
        case CameraControllerRuntime::Cluster::SessionState::Established: return State::Established;
        case CameraControllerRuntime::Cluster::SessionState::Closed: return State::Closed;
        case CameraControllerRuntime::Cluster::SessionState::Failed: return State::Failed;
        }
        throw hresult_illegal_method_call(L"Unknown native camera state.");
#else
        throw hresult_not_implemented(L"Camera signaling is not compiled into this component.");
#endif
    }

    void CloseCamera(uint64_t handle)
    {
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        auto lifetime = mAdmission.AcquireCleanup();
        if (!lifetime.owns_lock())
        {
            return;
        }
        if (mClosed || !mCameraHost)
        {
            return;
        }
        uint64_t node;
        {
            CameraStackLock lock;
            if (mCameraHost->Validate(handle) != CHIP_NO_ERROR)
            {
                return;
            }
            CheckChipError(mCameraHost->GetPeerNode(handle, node), L"Read camera cleanup peer");
        }
        const auto deadline = std::chrono::steady_clock::now() + kInteractionTimeout;
        auto operation = mAdmission.AcquireNodeWithLifetime(std::move(lifetime), node, {}, deadline);
        auto cameraLock = mAdmission.AcquireGate(mCameraOperationMutex, {}, deadline);
        {
            CameraStackLock lock;
            if (mCameraHost->Validate(handle) != CHIP_NO_ERROR)
            {
                return;
            }
        }
        const auto error = mCameraHost->Send(mCommissioner, handle, CameraControllerRuntime::Client::Command::End, {}, {}, {});
        {
            CameraStackLock lock;
            CheckChipError(mCameraHost->CloseLocal(handle), L"Close local camera session");
        }
        CheckChipError(error, L"End remote camera session");
#else
        throw hresult_not_implemented(L"Camera signaling is not compiled into this component.");
#endif
    }

    void WriteAttribute(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId, AttributeId attributeId,
                        Windows::Foundation::Collections::IPropertySet const & value,
                        std::optional<uint16_t> timedWriteTimeout = std::nullopt)
    {
        std::vector<uint8_t> encoded;
        CheckChipError(EncodePropertySetRoot(value, encoded), L"Encode Matter attribute value");
        auto operation = mAdmission.AcquireNode(nodeId);
        EnsureOpen();
        GenericWriteState state(endpointId, clusterId, attributeId, std::move(encoded), timedWriteTimeout);
        auto cleanup = chip::ScopeExit([&] { state.ReleaseClient(); });
        StartConnectedOperation(nodeId, state.onConnected, state.onConnectionFailure);
        std::unique_lock lock(state.mutex);
        if (!state.condition.wait_for(lock, InteractionWaitTimeout(timedWriteTimeout), [&state]() { return state.complete; }))
        {
            lock.unlock();
            state.ReleaseClient();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter attribute write timed out.");
        }
        CHIP_ERROR result = state.result;
        lock.unlock();
        state.ReleaseClient();
        CheckChipError(result, L"Write Matter attribute");
    }

    Windows::Foundation::Collections::IPropertySet InvokeCommand(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId,
                                                                  CommandId commandId,
                                                                  Windows::Foundation::Collections::IPropertySet const & arguments,
                                                                  std::optional<uint16_t> timedInvokeTimeout = std::nullopt,
                                                                  std::optional<CommandId> responseCommand = std::nullopt,
                                                                  bool responseData = false,
                                                                  Controller::MatterCameraStage * allocationStage = nullptr)
    {
        if (allocationStage != nullptr)
        {
            *allocationStage = Controller::MatterCameraStage::Validation;
        }
        std::vector<uint8_t> encoded;
        CheckChipError(EncodePropertySetRoot(arguments, encoded), L"Encode Matter command arguments");
        auto operation = mAdmission.AcquireNode(nodeId);
        EnsureOpen();
        GenericInvokeState state(endpointId, clusterId, commandId, std::move(encoded), timedInvokeTimeout, responseCommand,
                                 responseData);
        auto cleanup = chip::ScopeExit([&] { state.ReleaseSender(); });
        if (allocationStage != nullptr)
        {
            // Once connection/dispatch begins, conservatively preserve uncertain allocation ownership on failure.
            *allocationStage = Controller::MatterCameraStage::StreamAllocation;
        }
        StartConnectedOperation(nodeId, state.onConnected, state.onConnectionFailure);
        std::unique_lock lock(state.mutex);
        if (!state.condition.wait_for(lock, InteractionWaitTimeout(timedInvokeTimeout), [&state]() { return state.complete; }))
        {
            lock.unlock();
            state.ReleaseSender();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter command timed out.");
        }
        CHIP_ERROR result = state.result;
        auto value        = state.value;
        lock.unlock();
        state.ReleaseSender();
        CheckChipError(result, L"Invoke Matter command");
        return value ? value : Windows::Foundation::Collections::PropertySet();
    }

    Controller::AttributeSubscription SubscribeAttribute(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId,
                                                          AttributeId attributeId, uint16_t minimumInterval,
                                                          uint16_t maximumInterval)
    {
        if (minimumInterval > maximumInterval)
        {
            throw hresult_invalid_argument(L"The minimum subscription interval cannot exceed the maximum interval.");
        }
        auto operation = mAdmission.AcquireNode(nodeId);
        EnsureOpen();
        auto projectedWeak = std::make_shared<winrt::weak_ref<Controller::AttributeSubscription>>();
        auto state         = std::make_shared<GenericReadState>(
            endpointId, clusterId, attributeId, true, minimumInterval, maximumInterval,
            [projectedWeak](app::ConcreteDataAttributePath const & path,
                            Windows::Foundation::Collections::IPropertySet const & data) {
                if (auto subscription = projectedWeak->get())
                {
                    auto projectedPath =
                        winrt::make<implementation::AttributePath>(path.mEndpointId, path.mClusterId, path.mAttributeId);
                    auto value = winrt::make<implementation::AttributeValue>(projectedPath, data);
                    auto args  = winrt::make<implementation::AttributeReportEventArgs>(value);
                    winrt::get_self<implementation::AttributeSubscription>(subscription)->Publish(args);
                }
            });
        bool isRetained = false;
        auto cleanup = chip::ScopeExit([&] {
            if (!isRetained)
            {
                state->Close();
            }
        });
        auto runtimeWeak  = weak_from_this();
        auto subscription = winrt::make<implementation::AttributeSubscription>([runtimeWeak, state]() {
            if (auto runtime = runtimeWeak.lock())
            {
                runtime->CloseAttributeSubscription(state);
            }
            else
            {
                state->Close();
            }
        });
        *projectedWeak     = winrt::make_weak(subscription);
        StartConnectedOperation(nodeId, state->onConnected, state->onConnectionFailure);

        std::unique_lock lock(state->mutex);
        if (!state->condition.wait_for(lock, kInteractionTimeout,
                                       [&state]() { return state->established || state->complete; }))
        {
            lock.unlock();
            state->Close();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter subscription timed out.");
        }
        CHIP_ERROR result = state->result;
        bool established  = state->established;
        lock.unlock();
        if (result != CHIP_NO_ERROR)
        {
            state->Close();
        }
        CheckChipError(result, L"Subscribe to Matter attribute");
        if (!established)
        {
            state->Close();
            throw hresult_error(E_FAIL, L"The Matter subscription ended before it was established.");
        }
        {
            std::scoped_lock subscriptionLock(mOperationMutex);
            mSubscriptions.push_back(state);
        }
        isRetained = true;
        return subscription;
    }

    std::vector<Controller::EventValue> ReadEvents(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId, EventId eventId,
                                                   bool urgent, EventNumber minimumEventNumber)
    {
        auto operation = mAdmission.AcquireNode(nodeId);
        EnsureOpen();
        auto state = std::make_shared<GenericEventReadState>(endpointId, clusterId, eventId, urgent, minimumEventNumber, false, 0, 0);
        auto cleanup = chip::ScopeExit([&] { state->Close(); });
        StartConnectedOperation(nodeId, state->onConnected, state->onConnectionFailure);
        std::unique_lock lock(state->mutex);
        if (!state->condition.wait_for(lock, kInteractionTimeout, [&state]() { return state->complete; }))
        {
            lock.unlock();
            state->Close();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter event read timed out.");
        }
        CHIP_ERROR result = state->result;
        auto values       = std::move(state->values);
        lock.unlock();
        state->Close();
        CheckChipError(result, L"Read Matter events");
        return values;
    }

    Controller::EventSubscription SubscribeEvent(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId, EventId eventId,
                                                 bool urgent, EventNumber minimumEventNumber, uint16_t minimumInterval,
                                                 uint16_t maximumInterval)
    {
        if (minimumInterval > maximumInterval)
        {
            throw hresult_invalid_argument(L"The minimum subscription interval cannot exceed the maximum interval.");
        }
        auto operation = mAdmission.AcquireNode(nodeId);
        EnsureOpen();
        auto projectedWeak = std::make_shared<winrt::weak_ref<Controller::EventSubscription>>();
        auto state = std::make_shared<GenericEventReadState>(
            endpointId, clusterId, eventId, urgent, minimumEventNumber, true, minimumInterval, maximumInterval,
            [projectedWeak](Controller::EventValue const & value) {
                if (auto subscription = projectedWeak->get())
                {
                    auto args = winrt::make<implementation::EventReportEventArgs>(value);
                    winrt::get_self<implementation::EventSubscription>(subscription)->Publish(args);
                }
            });
        bool isRetained = false;
        auto cleanup = chip::ScopeExit([&] {
            if (!isRetained)
            {
                state->Close();
            }
        });
        auto runtimeWeak  = weak_from_this();
        auto subscription = winrt::make<implementation::EventSubscription>([runtimeWeak, state]() {
            if (auto runtime = runtimeWeak.lock())
            {
                runtime->CloseEventSubscription(state);
            }
            else
            {
                state->Close();
            }
        });
        *projectedWeak     = winrt::make_weak(subscription);
        StartConnectedOperation(nodeId, state->onConnected, state->onConnectionFailure);

        std::unique_lock lock(state->mutex);
        if (!state->condition.wait_for(lock, kInteractionTimeout,
                                       [&state]() { return state->established || state->complete; }))
        {
            lock.unlock();
            state->Close();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter event subscription timed out.");
        }
        CHIP_ERROR result = state->result;
        bool established  = state->established;
        lock.unlock();
        if (result != CHIP_NO_ERROR)
        {
            state->Close();
        }
        CheckChipError(result, L"Subscribe to Matter event");
        if (!established)
        {
            state->Close();
            throw hresult_error(E_FAIL, L"The Matter event subscription ended before it was established.");
        }
        {
            std::scoped_lock subscriptionLock(mOperationMutex);
            mEventSubscriptions.push_back(state);
        }
        isRetained = true;
        return subscription;
    }

private:
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
    uint64_t CameraPeerNode(uint64_t handle)
    {
        EnsureOpen();
        CameraStackLock lock;
        if (!mCameraHost)
        {
            throw hresult_illegal_method_call(L"Camera signaling is disabled.");
        }
        uint64_t node;
        CheckChipError(mCameraHost->GetPeerNode(handle, node), L"Read camera signaling peer");
        return node;
    }
#endif

    void EnsureOpen()
    {
        std::scoped_lock lock(mMutex);
        if (mClosed)
        {
            throw hresult_illegal_method_call(L"The Matter controller is closed.");
        }
    }

    void CloseAttributeSubscription(std::shared_ptr<GenericReadState> const & state)
    {
        auto lifetime = mAdmission.AcquireCleanup();
        if (!lifetime.owns_lock())
        {
            return;
        }
        state->Close();
        std::scoped_lock operationLock(mOperationMutex);
        mSubscriptions.erase(std::remove(mSubscriptions.begin(), mSubscriptions.end(), state), mSubscriptions.end());
    }

    void CloseEventSubscription(std::shared_ptr<GenericEventReadState> const & state)
    {
        auto lifetime = mAdmission.AcquireCleanup();
        if (!lifetime.owns_lock())
        {
            return;
        }
        state->Close();
        std::scoped_lock operationLock(mOperationMutex);
        mEventSubscriptions.erase(std::remove(mEventSubscriptions.begin(), mEventSubscriptions.end(), state),
                                  mEventSubscriptions.end());
    }

    explicit ControllerRuntime(Controller::ControllerOptions const & options) :
        mStoragePath(options.StoragePath()), mControllerNodeId(options.ControllerNodeId()),
        mBluetoothAdapterId(options.BluetoothAdapterId()), mAllowTestAttestation(options.AllowTestAttestation()),
        mEnableCameraSignaling(options.EnableCameraSignaling()),
        mGroupDataProvider(50, 25), mPairingDelegate(mPairingState, mAllowTestAttestation)
    {}

    void Initialize()
    {
        std::filesystem::path storagePath(mStoragePath.c_str());
        if (!storagePath.is_absolute())
        {
            throw hresult_invalid_argument(L"ControllerOptions.StoragePath must be absolute.");
        }

        std::filesystem::create_directories(storagePath);
        std::string storageUtf8 = to_string(mStoragePath);

        CheckChipError(Platform::MemoryInit(), L"Initialize Matter memory");
        CheckChipError(DeviceLayer::Internal::BLEMgrImpl().ConfigureBle(mBluetoothAdapterId, true), L"Configure Bluetooth LE");
        CheckChipError(ConfigurationManagerImpl::GetDefaultInstance().ConfigureStorageRoot(storageUtf8.c_str()),
                       L"Configure controller storage");
        CheckChipError(PlatformMgr().InitChipStack(), L"Initialize Matter platform");
        CheckChipError(PlatformMgr().StartEventLoopTask(), L"Start Matter event loop");

        PlatformMgr().LockChipStack();
        CHIP_ERROR error = InitializeController();
        PlatformMgr().UnlockChipStack();
        if (error != CHIP_NO_ERROR)
        {
            Close();
            ThrowChipError(error, L"Initialize Matter controller");
        }
    }

    void StartConnectedOperation(uint64_t nodeId, chip::Callback::Callback<OnDeviceConnected> & onConnected,
                                 chip::Callback::Callback<OnDeviceConnectionFailure> & onConnectionFailure)
    {
        PlatformMgr().LockChipStack();
        CHIP_ERROR error = mCommissioner.GetConnectedDevice(nodeId, &onConnected, &onConnectionFailure);
        PlatformMgr().UnlockChipStack();
        CheckChipError(error, L"Start CASE session");
    }

    CHIP_ERROR InitializeController()
    {
        ReturnErrorOnFailure(mStorage.Init(&PersistedStorage::KeyValueStoreMgr()));
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        if (mEnableCameraSignaling)
        {
            mCameraHost = std::make_unique<CameraControllerRuntime>(mStorage);
            ReturnErrorOnFailure(mCameraHost->Init(mStorage));
        }
#else
        VerifyOrReturnError(!mEnableCameraSignaling, CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
#endif
        LoadCommissionedNodes();
        ReturnErrorOnFailure(mOperationalKeystore.Init(&mStorage));
        ReturnErrorOnFailure(mOpCertStore.Init(&mStorage));

        mGroupDataProvider.SetStorageDelegate(&mStorage);
        mGroupDataProvider.SetSessionKeystore(&mSessionKeystore);
        ReturnErrorOnFailure(mGroupDataProvider.Init());
        Credentials::SetGroupDataProvider(&mGroupDataProvider);

        FactoryInitParams factoryParameters;
        factoryParameters.fabricIndependentStorage = &mStorage;
        factoryParameters.operationalKeystore      = &mOperationalKeystore;
        factoryParameters.opCertStore              = &mOpCertStore;
        factoryParameters.sessionKeystore          = &mSessionKeystore;
        factoryParameters.groupDataProvider        = &mGroupDataProvider;
        factoryParameters.dataModelProvider        = &mDataModelProvider;
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        if (mCameraHost)
        {
            factoryParameters.dataModelProvider = &mCameraHost->Provider();
            factoryParameters.enableServerInteractions = true;
            factoryParameters.enableTCPServer = true;
            // Port zero binds UDP and TCP independently; the advertised port must reach both.
            factoryParameters.listenPort = CHIP_PORT;
        }
#endif
        ReturnErrorOnFailure(DeviceControllerFactory::GetInstance().Init(factoryParameters));
        mFactoryInitialized = true;

#pragma warning(suppress : 4996)
        ReturnErrorOnFailure(mCredentialsIssuer.Initialize(mStorage));
        SetupParams commissionerParameters;
        commissionerParameters.operationalCredentialsDelegate = &mCredentialsIssuer;
        commissionerParameters.controllerVendorId             = VendorId::TestVendor1;
        commissionerParameters.pairingDelegate                 = &mPairingDelegate;
        commissionerParameters.deviceAttestationVerifier =
            Credentials::GetDefaultDACVerifier(Credentials::GetTestAttestationTrustStore(), nullptr);

        FabricTable * fabrics = DeviceControllerFactory::GetInstance().GetSystemState()->Fabrics();
        VerifyOrReturnError(fabrics != nullptr, CHIP_ERROR_INCORRECT_STATE);
        bool restored = false;
        if (fabrics->FabricCount() > 0)
        {
            chip::FabricIndex fabricIndex = fabrics->begin()->GetFabricIndex();
            if (fabrics->HasOperationalKeyForFabric(fabricIndex))
            {
                commissionerParameters.fabricIndex.SetValue(fabricIndex);
                restored = true;
            }
            else
            {
                ReturnErrorOnFailure(fabrics->Delete(fabricIndex));
            }
        }

        if (!restored)
        {
            uint8_t csrBuffer[Crypto::kMIN_CSR_Buffer_Size];
            uint8_t nocBuffer[kMaxCHIPDERCertLength];
            uint8_t icacBuffer[kMaxCHIPDERCertLength];
            uint8_t rcacBuffer[kMaxCHIPDERCertLength];
            MutableByteSpan csr(csrBuffer);
            MutableByteSpan noc(nocBuffer);
            MutableByteSpan icac(icacBuffer);
            MutableByteSpan rcac(rcacBuffer);
            Crypto::P256PublicKey operationalPublicKey;

            ReturnErrorOnFailure(fabrics->AllocatePendingOperationalKey(NullOptional, csr));
            ReturnErrorOnFailure(Crypto::VerifyCertificateSigningRequest(csr.data(), csr.size(), operationalPublicKey));
            ReturnErrorOnFailure(mCredentialsIssuer.GenerateNOCChainAfterValidation(
                mControllerNodeId, kControllerFabricId, kUndefinedCATs, operationalPublicKey, rcac, icac, noc));
            commissionerParameters.controllerRCAC = rcac;
            commissionerParameters.controllerICAC = icac;
            commissionerParameters.controllerNOC  = noc;
        }

        ReturnErrorOnFailure(DeviceControllerFactory::GetInstance().SetupCommissioner(commissionerParameters, mCommissioner));
        mCommissionerInitialized = true;

        uint8_t compressedFabricId[sizeof(uint64_t)];
        MutableByteSpan compressedFabricIdSpan(compressedFabricId);
        ReturnErrorOnFailure(mCommissioner.GetCompressedFabricIdBytes(compressedFabricIdSpan));
        return Credentials::SetSingleIpkEpochKey(&mGroupDataProvider, mCommissioner.GetFabricIndex(),
                                                 GroupTesting::DefaultIpkValue::GetDefaultIpk(), compressedFabricIdSpan);
    }

    void LoadCommissionedNodes()
    {
        constexpr size_t kMaximumNodeCount = UINT16_MAX / sizeof(uint64_t);
        std::vector<uint64_t> nodes(1);
        while (true)
        {
            uint16_t size = static_cast<uint16_t>(nodes.size() * sizeof(uint64_t));
            CHIP_ERROR error = mStorage.SyncGetKeyValue(kCommissionedNodesKey, nodes.data(), size);
            if (error == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)
            {
                return;
            }
            if (error == CHIP_ERROR_BUFFER_TOO_SMALL)
            {
                if (nodes.size() == kMaximumNodeCount)
                {
                    throw hresult_error(E_BOUNDS, L"The commissioned node index is too large.");
                }
                nodes.resize(std::min(nodes.size() * 2, kMaximumNodeCount));
                continue;
            }
            CheckChipError(error, L"Read commissioned node index");
            if (size == 0 || size % sizeof(uint64_t) != 0)
            {
                throw hresult_error(E_UNEXPECTED, L"The commissioned node index is invalid.");
            }
            nodes.resize(size / sizeof(uint64_t));
            mCommissionedNodes = std::move(nodes);
            return;
        }
    }

    void PersistCommissionedNodes()
    {
        if (mCommissionedNodes.empty())
        {
            CHIP_ERROR error = mStorage.SyncDeleteKeyValue(kCommissionedNodesKey);
            if (error != CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)
            {
                CheckChipError(error, L"Delete commissioned node index");
            }
            return;
        }
        size_t byteCount = mCommissionedNodes.size() * sizeof(uint64_t);
        if (byteCount > UINT16_MAX)
        {
            throw hresult_error(E_BOUNDS, L"Too many commissioned nodes are stored.");
        }
        CheckChipError(mStorage.SyncSetKeyValue(kCommissionedNodesKey, mCommissionedNodes.data(), static_cast<uint16_t>(byteCount)),
                       L"Write commissioned node index");
    }

    static std::mutex sMutex;
    static std::weak_ptr<ControllerRuntime> sInstance;

    std::mutex mMutex;
    std::timed_mutex mOperationMutex;
    RuntimeOperationAdmission mAdmission;
    std::timed_mutex mCameraOperationMutex;
    CameraAudioAllocationOwnership mAudioOwnership;
    hstring mStoragePath;
    uint64_t mControllerNodeId;
    uint32_t mBluetoothAdapterId;
    bool mAllowTestAttestation;
    bool mEnableCameraSignaling;
    bool mClosed = false;
    KvsPersistentStorageDelegate mStorage;
    PersistentStorageOperationalKeystore mOperationalKeystore;
    Credentials::PersistentStorageOpCertStore mOpCertStore;
    Crypto::RawKeySessionKeystore mSessionKeystore;
    Credentials::GroupDataProviderImpl mGroupDataProvider;
    ExampleOperationalCredentialsIssuer mCredentialsIssuer;
    Testing::EmptyProvider mDataModelProvider;
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
    std::unique_ptr<CameraControllerRuntime> mCameraHost;
#endif
    DeviceCommissioner mCommissioner;
    PairingState mPairingState;
    PairingDelegate mPairingDelegate;
    bool mFactoryInitialized = false;
    bool mCommissionerInitialized = false;
    std::vector<uint64_t> mCommissionedNodes;
    std::vector<std::shared_ptr<GenericReadState>> mSubscriptions;
    std::vector<std::shared_ptr<GenericEventReadState>> mEventSubscriptions;
};

std::mutex ControllerRuntime::sMutex;
std::weak_ptr<ControllerRuntime> ControllerRuntime::sInstance;

hstring ControllerOptions::StoragePath() const
{
    return mStoragePath;
}

void ControllerOptions::StoragePath(hstring const & value)
{
    mStoragePath = value;
}

uint64_t ControllerOptions::ControllerNodeId() const
{
    return mControllerNodeId;
}

void ControllerOptions::ControllerNodeId(uint64_t value)
{
    mControllerNodeId = value;
}

uint32_t ControllerOptions::BluetoothAdapterId() const
{
    return mBluetoothAdapterId;
}

void ControllerOptions::BluetoothAdapterId(uint32_t value)
{
    mBluetoothAdapterId = value;
}

bool ControllerOptions::AllowTestAttestation() const
{
    return mAllowTestAttestation;
}

void ControllerOptions::AllowTestAttestation(bool value)
{
    mAllowTestAttestation = value;
}

uint64_t OnNetworkCommissioningParameters::NodeId() const
{
    return mNodeId;
}

void OnNetworkCommissioningParameters::NodeId(uint64_t value)
{
    mNodeId = value;
}

hstring OnNetworkCommissioningParameters::SetupCode() const
{
    return mSetupCode;
}

void OnNetworkCommissioningParameters::SetupCode(hstring const & value)
{
    mSetupCode = value;
}

uint32_t OnNetworkCommissioningParameters::SetupPinCode() const
{
    return mSetupPinCode;
}

void OnNetworkCommissioningParameters::SetupPinCode(uint32_t value)
{
    mSetupPinCode = value;
}

uint16_t OnNetworkCommissioningParameters::LongDiscriminator() const
{
    return mLongDiscriminator;
}

void OnNetworkCommissioningParameters::LongDiscriminator(uint16_t value)
{
    mLongDiscriminator = value;
}

hstring OnNetworkCommissioningParameters::IpAddress() const
{
    return mIpAddress;
}

void OnNetworkCommissioningParameters::IpAddress(hstring const & value)
{
    mIpAddress = value;
}

uint64_t BleCommissioningParameters::NodeId() const
{
    return mNodeId;
}

void BleCommissioningParameters::NodeId(uint64_t value)
{
    mNodeId = value;
}

uint32_t BleCommissioningParameters::SetupPinCode() const
{
    return mSetupPinCode;
}

void BleCommissioningParameters::SetupPinCode(uint32_t value)
{
    mSetupPinCode = value;
}

uint16_t BleCommissioningParameters::LongDiscriminator() const
{
    return mLongDiscriminator;
}

void BleCommissioningParameters::LongDiscriminator(uint16_t value)
{
    mLongDiscriminator = value;
}

WiFiNetworkCredentials::WiFiNetworkCredentials(hstring const & ssid, hstring const & passphrase)
{
    std::vector<uint8_t> encodedSsid       = Utf8Bytes(ssid);
    std::vector<uint8_t> encodedPassphrase = Utf8Bytes(passphrase);
    if (encodedSsid.empty() || encodedSsid.size() > CommissioningParameters::kMaxSsidLen ||
        encodedPassphrase.size() > CommissioningParameters::kMaxCredentialsLen)
    {
        Crypto::ClearSecretData(encodedSsid.data(), encodedSsid.size());
        Crypto::ClearSecretData(encodedPassphrase.data(), encodedPassphrase.size());
        throw hresult_invalid_argument(
            L"ssid must contain 1 to 32 UTF-8 bytes and passphrase must not exceed 64 UTF-8 bytes.");
    }
    mSsid       = std::move(encodedSsid);
    mPassphrase = std::move(encodedPassphrase);
}

WiFiNetworkCredentials::~WiFiNetworkCredentials()
{
    if (!mSsid.empty())
    {
        Crypto::ClearSecretData(mSsid.data(), mSsid.size());
    }
    if (!mPassphrase.empty())
    {
        Crypto::ClearSecretData(mPassphrase.data(), mPassphrase.size());
    }
}

uint32_t WiFiNetworkCredentials::SsidLength() const
{
    return static_cast<uint32_t>(mSsid.size());
}

uint32_t WiFiNetworkCredentials::PassphraseLength() const
{
    return static_cast<uint32_t>(mPassphrase.size());
}

std::vector<uint8_t> const & WiFiNetworkCredentials::Ssid() const
{
    return mSsid;
}

std::vector<uint8_t> const & WiFiNetworkCredentials::Passphrase() const
{
    return mPassphrase;
}

ThreadNetworkCredentials::ThreadNetworkCredentials(Windows::Storage::Streams::IBuffer const & operationalDataset)
{
    if (!operationalDataset)
    {
        throw hresult_invalid_argument(L"operationalDataset cannot be null.");
    }
    if (operationalDataset.Length() == 0 ||
        operationalDataset.Length() > CommissioningParameters::kMaxThreadDatasetLen)
    {
        throw hresult_invalid_argument(L"operationalDataset must contain between 1 and 254 bytes.");
    }
    std::vector<uint8_t> dataset(operationalDataset.Length());
    try
    {
        Windows::Storage::Streams::DataReader::FromBuffer(operationalDataset).ReadBytes(dataset);
    }
    catch (...)
    {
        Crypto::ClearSecretData(dataset.data(), dataset.size());
        throw;
    }
    mOperationalDataset = std::move(dataset);
}

ThreadNetworkCredentials::~ThreadNetworkCredentials()
{
    if (!mOperationalDataset.empty())
    {
        Crypto::ClearSecretData(mOperationalDataset.data(), mOperationalDataset.size());
    }
}

uint32_t ThreadNetworkCredentials::DatasetLength() const
{
    return static_cast<uint32_t>(mOperationalDataset.size());
}

std::vector<uint8_t> const & ThreadNetworkCredentials::OperationalDataset() const
{
    return mOperationalDataset;
}

uint64_t BleNetworkCommissioningParameters::NodeId() const
{
    return mNodeId;
}

void BleNetworkCommissioningParameters::NodeId(uint64_t value)
{
    mNodeId = value;
}

uint32_t BleNetworkCommissioningParameters::SetupPinCode() const
{
    return mSetupPinCode;
}

void BleNetworkCommissioningParameters::SetupPinCode(uint32_t value)
{
    mSetupPinCode = value;
}

uint16_t BleNetworkCommissioningParameters::LongDiscriminator() const
{
    return mLongDiscriminator;
}

void BleNetworkCommissioningParameters::LongDiscriminator(uint16_t value)
{
    mLongDiscriminator = value;
}

Controller::WiFiNetworkCredentials BleNetworkCommissioningParameters::WiFi() const
{
    return mWiFi;
}

void BleNetworkCommissioningParameters::WiFi(Controller::WiFiNetworkCredentials const & value)
{
    mWiFi = value;
}

Controller::ThreadNetworkCredentials BleNetworkCommissioningParameters::Thread() const
{
    return mThread;
}

void BleNetworkCommissioningParameters::Thread(Controller::ThreadNetworkCredentials const & value)
{
    mThread = value;
}

AttributePath::AttributePath(uint16_t endpointId, uint32_t clusterId, uint32_t attributeId) :
    mEndpointId(endpointId), mClusterId(clusterId), mAttributeId(attributeId)
{}

uint16_t AttributePath::EndpointId() const
{
    return mEndpointId;
}

uint32_t AttributePath::ClusterId() const
{
    return mClusterId;
}

uint32_t AttributePath::AttributeId() const
{
    return mAttributeId;
}

CommandPath::CommandPath(uint16_t endpointId, uint32_t clusterId, uint32_t commandId) :
    mEndpointId(endpointId), mClusterId(clusterId), mCommandId(commandId)
{}

uint16_t CommandPath::EndpointId() const
{
    return mEndpointId;
}

uint32_t CommandPath::ClusterId() const
{
    return mClusterId;
}

uint32_t CommandPath::CommandId() const
{
    return mCommandId;
}

EventPath::EventPath(uint16_t endpointId, uint32_t clusterId, uint32_t eventId, bool urgent) :
    mEndpointId(endpointId), mClusterId(clusterId), mEventId(eventId), mUrgent(urgent)
{}

uint16_t EventPath::EndpointId() const
{
    return mEndpointId;
}

uint32_t EventPath::ClusterId() const
{
    return mClusterId;
}

uint32_t EventPath::EventId() const
{
    return mEventId;
}

bool EventPath::Urgent() const
{
    return mUrgent;
}

CommissionedNode::CommissionedNode(uint64_t nodeId, uint16_t fabricIndex) : mNodeId(nodeId), mFabricIndex(fabricIndex) {}

uint64_t CommissionedNode::NodeId() const
{
    return mNodeId;
}

uint16_t CommissionedNode::FabricIndex() const
{
    return mFabricIndex;
}

CommissioningProgressEventArgs::CommissioningProgressEventArgs(Controller::CommissioningStage stage, hstring message) :
    mStage(stage), mMessage(std::move(message))
{}

Controller::CommissioningStage CommissioningProgressEventArgs::Stage() const
{
    return mStage;
}

hstring CommissioningProgressEventArgs::Message() const
{
    return mMessage;
}

MatterNetworkInterfaceSelection::MatterNetworkInterfaceSelection(Controller::MatterNetworkInterfaceSelectionMode mode,
                                                                 uint64_t interfaceId) :
    mMode(mode),
    mInterfaceId(interfaceId)
{
    if ((mode == Controller::MatterNetworkInterfaceSelectionMode::Automatic) != (interfaceId == 0))
    {
        throw hresult_invalid_argument(L"Automatic selection requires interface ID 0; explicit selection requires a nonzero ID.");
    }
}

Controller::MatterNetworkInterfaceSelectionMode MatterNetworkInterfaceSelection::Mode() const
{
    return mMode;
}

uint64_t MatterNetworkInterfaceSelection::InterfaceId() const
{
    return mInterfaceId;
}

MatterNetworkInterface::MatterNetworkInterface(uint64_t id, uint32_t index, hstring name,
                                               Controller::MatterNetworkInterfaceType type, bool connected,
                                               bool supportsIpv6, bool supportsMulticast, bool isVirtual) :
    mId(id),
    mIndex(index), mName(std::move(name)), mType(type), mConnected(connected), mSupportsIpv6(supportsIpv6),
    mSupportsMulticast(supportsMulticast), mIsVirtual(isVirtual)
{}

uint64_t MatterNetworkInterface::Id() const { return mId; }
uint32_t MatterNetworkInterface::InterfaceIndex() const { return mIndex; }
hstring MatterNetworkInterface::Name() const { return mName; }
Controller::MatterNetworkInterfaceType MatterNetworkInterface::Type() const { return mType; }
bool MatterNetworkInterface::IsConnected() const { return mConnected; }
bool MatterNetworkInterface::SupportsIpv6() const { return mSupportsIpv6; }
bool MatterNetworkInterface::SupportsMulticast() const { return mSupportsMulticast; }
bool MatterNetworkInterface::IsVirtual() const { return mIsVirtual; }

MatterCommissioningProgress::MatterCommissioningProgress(
    Controller::MatterCommissioningStage stage, Controller::MatterCommissioningStage lastCompletedStage, int32_t nativeStageId,
    Controller::MatterCommissioningTransport transport, Windows::Foundation::TimeSpan elapsedTime, hstring diagnosticMessage,
    hstring displayMessage, uint64_t networkInterfaceId, hstring networkInterfaceName, uint32_t attemptNumber, bool isRetrying) :
    mStage(stage),
    mLastCompletedStage(lastCompletedStage), mNativeStageId(nativeStageId), mTransport(transport), mElapsedTime(elapsedTime),
    mDiagnosticMessage(std::move(diagnosticMessage)), mDisplayMessage(std::move(displayMessage)),
    mNetworkInterfaceId(networkInterfaceId), mNetworkInterfaceName(std::move(networkInterfaceName)),
    mAttemptNumber(attemptNumber), mIsRetrying(isRetrying)
{}

Controller::MatterCommissioningStage MatterCommissioningProgress::Stage() const { return mStage; }
Controller::MatterCommissioningStage MatterCommissioningProgress::LastCompletedStage() const { return mLastCompletedStage; }
int32_t MatterCommissioningProgress::NativeStageId() const { return mNativeStageId; }
Controller::MatterCommissioningTransport MatterCommissioningProgress::Transport() const { return mTransport; }
Windows::Foundation::TimeSpan MatterCommissioningProgress::ElapsedTime() const { return mElapsedTime; }
hstring MatterCommissioningProgress::DiagnosticMessage() const { return mDiagnosticMessage; }
hstring MatterCommissioningProgress::DisplayMessage() const { return mDisplayMessage; }
uint64_t MatterCommissioningProgress::NetworkInterfaceId() const { return mNetworkInterfaceId; }
hstring MatterCommissioningProgress::NetworkInterfaceName() const { return mNetworkInterfaceName; }
uint32_t MatterCommissioningProgress::AttemptNumber() const { return mAttemptNumber; }
bool MatterCommissioningProgress::IsRetrying() const { return mIsRetrying; }

MatterCommissioningResult::MatterCommissioningResult(
    bool succeeded, Controller::MatterCommissioningOutcome outcome, Controller::MatterCommissioningFailureKind failureKind,
    Controller::MatterCommissioningStage failedStage, Controller::MatterCommissioningStage lastCompletedStage,
    int32_t nativeStageId, int32_t nativeErrorCode, hstring diagnosticMessage, uint64_t networkInterfaceId,
    Controller::CommissionedNode node) :
    mSucceeded(succeeded),
    mOutcome(outcome), mFailureKind(failureKind), mFailedStage(failedStage), mLastCompletedStage(lastCompletedStage),
    mNativeStageId(nativeStageId), mNativeErrorCode(nativeErrorCode), mDiagnosticMessage(std::move(diagnosticMessage)),
    mNetworkInterfaceId(networkInterfaceId), mNode(std::move(node))
{}

bool MatterCommissioningResult::Succeeded() const { return mSucceeded; }
Controller::MatterCommissioningOutcome MatterCommissioningResult::Outcome() const { return mOutcome; }
Controller::MatterCommissioningFailureKind MatterCommissioningResult::FailureKind() const { return mFailureKind; }
Controller::MatterCommissioningStage MatterCommissioningResult::FailedStage() const { return mFailedStage; }
Controller::MatterCommissioningStage MatterCommissioningResult::LastCompletedStage() const { return mLastCompletedStage; }
int32_t MatterCommissioningResult::NativeStageId() const { return mNativeStageId; }
int32_t MatterCommissioningResult::NativeErrorCode() const { return mNativeErrorCode; }
hstring MatterCommissioningResult::DiagnosticMessage() const { return mDiagnosticMessage; }
uint64_t MatterCommissioningResult::NetworkInterfaceId() const { return mNetworkInterfaceId; }
Controller::CommissionedNode MatterCommissioningResult::Node() const { return mNode; }

AttributeValue::AttributeValue(Controller::AttributePath path, Windows::Foundation::Collections::IPropertySet data) :
    mPath(std::move(path)), mData(std::move(data))
{}

Controller::AttributePath AttributeValue::Path() const
{
    return mPath;
}

Windows::Foundation::Collections::IPropertySet AttributeValue::Data() const
{
    return mData;
}

CommandResult::CommandResult(Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet data) :
    mPath(std::move(path)), mData(std::move(data))
{}

Controller::CommandPath CommandResult::Path() const
{
    return mPath;
}

Windows::Foundation::Collections::IPropertySet CommandResult::Data() const
{
    return mData;
}

TimedInteractionOptions::TimedInteractionOptions(uint16_t timeoutMilliseconds) :
    mTimeoutMilliseconds(timeoutMilliseconds)
{
    if (timeoutMilliseconds == 0)
    {
        throw hresult_invalid_argument(L"The timed interaction timeout must be greater than zero.");
    }
}

uint16_t TimedInteractionOptions::TimeoutMilliseconds() const
{
    return mTimeoutMilliseconds;
}

EventValue::EventValue(Controller::EventPath path, uint64_t eventNumber,
                       Windows::Foundation::Collections::IPropertySet data) :
    mPath(std::move(path)), mEventNumber(eventNumber), mData(std::move(data))
{}

Controller::EventPath EventValue::Path() const
{
    return mPath;
}

uint64_t EventValue::EventNumber() const
{
    return mEventNumber;
}

Windows::Foundation::Collections::IPropertySet EventValue::Data() const
{
    return mData;
}

EventReportEventArgs::EventReportEventArgs(Controller::EventValue value) : mValue(std::move(value)) {}

Controller::EventValue EventReportEventArgs::Value() const
{
    return mValue;
}

EventSubscription::EventSubscription(std::function<void()> close) : mClose(std::move(close)) {}

event_token EventSubscription::ReportReceived(
    Windows::Foundation::TypedEventHandler<Controller::EventSubscription, Controller::EventReportEventArgs> const & handler)
{
    std::vector<Controller::EventReportEventArgs> pendingReports;
    std::exception_ptr handlerError;
    event_token token;
    {
        std::scoped_lock lock(mReportMutex);
        token = mReportReceived.add(handler);
        ++mReportHandlerCount;
        if (mReportHandlerCount > 1)
        {
            return token;
        }
        mDrainingPendingReports = true;
        pendingReports.swap(mPendingReports);
    }
    while (true)
    {
        for (auto const & report : pendingReports)
        {
            try
            {
                mReportReceived(*this, report);
            }
            catch (...)
            {
                if (!handlerError)
                {
                    handlerError = std::current_exception();
                }
            }
        }
        std::scoped_lock lock(mReportMutex);
        if (mPendingReports.empty())
        {
            mDrainingPendingReports = false;
            break;
        }
        pendingReports.clear();
        pendingReports.swap(mPendingReports);
    }
    if (handlerError)
    {
        std::scoped_lock lock(mReportMutex);
        mReportReceived.remove(token);
        --mReportHandlerCount;
        std::rethrow_exception(handlerError);
    }
    return token;
}

void EventSubscription::ReportReceived(event_token const & token) noexcept
{
    std::scoped_lock lock(mReportMutex);
    mReportReceived.remove(token);
    if (mReportHandlerCount > 0)
    {
        --mReportHandlerCount;
    }
}

Windows::Foundation::IAsyncAction EventSubscription::CloseAsync()
{
    co_await resume_background();
    std::function<void()> close;
    {
        std::scoped_lock lock(mReportMutex);
        close = std::move(mClose);
    }
    if (close)
    {
        close();
    }
}

void EventSubscription::Publish(Controller::EventReportEventArgs const & args)
{
    {
        std::scoped_lock lock(mReportMutex);
        if (!mClose)
        {
            return;
        }
        if (mReportHandlerCount == 0 || mDrainingPendingReports)
        {
            if (mPendingReports.size() == kMaximumPendingReports)
            {
                mPendingReports.erase(mPendingReports.begin());
            }
            mPendingReports.push_back(args);
            return;
        }
    }
    mReportReceived(*this, args);
}

AttributeReportEventArgs::AttributeReportEventArgs(Controller::AttributeValue value) : mValue(std::move(value)) {}

Controller::AttributeValue AttributeReportEventArgs::Value() const
{
    return mValue;
}

AttributeSubscription::AttributeSubscription(std::function<void()> close) : mClose(std::move(close)) {}

event_token AttributeSubscription::ReportReceived(
    Windows::Foundation::TypedEventHandler<Controller::AttributeSubscription, Controller::AttributeReportEventArgs> const & handler)
{
    std::vector<Controller::AttributeReportEventArgs> pendingReports;
    std::exception_ptr handlerError;
    event_token token;
    {
        std::scoped_lock lock(mReportMutex);
        token = mReportReceived.add(handler);
        ++mReportHandlerCount;
        if (mReportHandlerCount > 1)
        {
            return token;
        }
        mDrainingPendingReports = true;
        pendingReports.swap(mPendingReports);
    }
    while (true)
    {
        for (auto const & report : pendingReports)
        {
            try
            {
                mReportReceived(*this, report);
            }
            catch (...)
            {
                if (!handlerError)
                {
                    handlerError = std::current_exception();
                }
            }
        }
        std::scoped_lock lock(mReportMutex);
        if (mPendingReports.empty())
        {
            mDrainingPendingReports = false;
            break;
        }
        pendingReports.clear();
        pendingReports.swap(mPendingReports);
    }
    if (handlerError)
    {
        std::scoped_lock lock(mReportMutex);
        mReportReceived.remove(token);
        --mReportHandlerCount;
        std::rethrow_exception(handlerError);
    }
    return token;
}

void AttributeSubscription::ReportReceived(event_token const & token) noexcept
{
    std::scoped_lock lock(mReportMutex);
    mReportReceived.remove(token);
    if (mReportHandlerCount > 0)
    {
        --mReportHandlerCount;
    }
}

Windows::Foundation::IAsyncAction AttributeSubscription::CloseAsync()
{
    co_await resume_background();
    std::function<void()> close;
    {
        std::scoped_lock lock(mReportMutex);
        close = std::move(mClose);
    }
    if (close)
    {
        close();
    }
}

void AttributeSubscription::Publish(Controller::AttributeReportEventArgs const & args)
{
    {
        std::scoped_lock lock(mReportMutex);
        if (!mClose)
        {
            return;
        }
        if (mReportHandlerCount == 0 || mDrainingPendingReports)
        {
            if (mPendingReports.size() == kMaximumPendingReports)
            {
                mPendingReports.erase(mPendingReports.begin());
            }
            mPendingReports.push_back(args);
            return;
        }
    }
    mReportReceived(*this, args);
}

BasicInformation::BasicInformation(uint16_t vendorId, hstring vendorName, uint16_t productId, hstring productName,
                                   hstring nodeLabel, hstring serialNumber, uint32_t softwareVersion,
                                   hstring softwareVersionString) :
    mVendorId(vendorId), mVendorName(std::move(vendorName)), mProductId(productId), mProductName(std::move(productName)),
    mNodeLabel(std::move(nodeLabel)), mSerialNumber(std::move(serialNumber)), mSoftwareVersion(softwareVersion),
    mSoftwareVersionString(std::move(softwareVersionString))
{}

uint16_t BasicInformation::VendorId() const
{
    return mVendorId;
}

hstring BasicInformation::VendorName() const
{
    return mVendorName;
}

uint16_t BasicInformation::ProductId() const
{
    return mProductId;
}

hstring BasicInformation::ProductName() const
{
    return mProductName;
}

hstring BasicInformation::NodeLabel() const
{
    return mNodeLabel;
}

hstring BasicInformation::SerialNumber() const
{
    return mSerialNumber;
}

uint32_t BasicInformation::SoftwareVersion() const
{
    return mSoftwareVersion;
}

hstring BasicInformation::SoftwareVersionString() const
{
    return mSoftwareVersionString;
}

OnOffCluster::OnOffCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId) :
    mRuntime(std::move(runtime)), mNodeId(nodeId), mEndpointId(endpointId)
{}

Windows::Foundation::IAsyncOperation<bool> OnOffCluster::ReadAsync()
{
    co_await resume_background();
    co_return mRuntime->RunOnOff(mNodeId, mEndpointId, OnOffOperation::Read);
}

Windows::Foundation::IAsyncAction OnOffCluster::SetAsync(bool value)
{
    co_await resume_background();
    mRuntime->RunOnOff(mNodeId, mEndpointId, value ? OnOffOperation::On : OnOffOperation::Off);
}

Windows::Foundation::IAsyncAction OnOffCluster::OnAsync()
{
    co_await resume_background();
    mRuntime->RunOnOff(mNodeId, mEndpointId, OnOffOperation::On);
}

Windows::Foundation::IAsyncAction OnOffCluster::OffAsync()
{
    co_await resume_background();
    mRuntime->RunOnOff(mNodeId, mEndpointId, OnOffOperation::Off);
}

Windows::Foundation::IAsyncAction OnOffCluster::ToggleAsync()
{
    co_await resume_background();
    mRuntime->RunOnOff(mNodeId, mEndpointId, OnOffOperation::Toggle);
}

Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription>
OnOffCluster::SubscribeAsync(uint16_t minimumIntervalSeconds, uint16_t maximumIntervalSeconds)
{
    co_await resume_background();
    co_return mRuntime->SubscribeAttribute(mNodeId, mEndpointId, app::Clusters::OnOff::Id,
                                           app::Clusters::OnOff::Attributes::OnOff::Id, minimumIntervalSeconds,
                                           maximumIntervalSeconds);
}

LevelControlCluster::LevelControlCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId) :
    mRuntime(std::move(runtime)), mNodeId(nodeId), mEndpointId(endpointId)
{}

Windows::Foundation::IAsyncOperation<uint8_t> LevelControlCluster::ReadCurrentLevelAsync()
{
    co_await resume_background();
    co_return PropertySetScalar<uint8_t>(
        mRuntime->ReadAttribute(mNodeId, mEndpointId, app::Clusters::LevelControl::Id,
                                app::Clusters::LevelControl::Attributes::CurrentLevel::Id),
        L"CurrentLevel");
}

Windows::Foundation::IAsyncAction LevelControlCluster::MoveToLevelAsync(uint8_t level, uint16_t transitionTime,
                                                                       uint8_t optionsMask, uint8_t optionsOverride)
{
    co_await resume_background();
    Windows::Foundation::Collections::PropertySet arguments;
    arguments.Insert(L"0", box_value(level));
    arguments.Insert(L"1", box_value(transitionTime));
    arguments.Insert(L"2", box_value(optionsMask));
    arguments.Insert(L"3", box_value(optionsOverride));
    mRuntime->InvokeCommand(mNodeId, mEndpointId, app::Clusters::LevelControl::Id,
                            app::Clusters::LevelControl::Commands::MoveToLevel::Id, arguments);
}

Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription>
LevelControlCluster::SubscribeAsync(uint16_t minimumIntervalSeconds, uint16_t maximumIntervalSeconds)
{
    co_await resume_background();
    co_return mRuntime->SubscribeAttribute(mNodeId, mEndpointId, app::Clusters::LevelControl::Id,
                                           app::Clusters::LevelControl::Attributes::CurrentLevel::Id, minimumIntervalSeconds,
                                           maximumIntervalSeconds);
}

BasicInformationCluster::BasicInformationCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId) :
    mRuntime(std::move(runtime)), mNodeId(nodeId), mEndpointId(endpointId)
{}

Windows::Foundation::IAsyncOperation<Controller::BasicInformation> BasicInformationCluster::ReadAsync()
{
    co_await resume_background();
    using namespace app::Clusters::BasicInformation::Attributes;
    auto readValue = [this](AttributeId attributeId) {
        return mRuntime->ReadAttribute(mNodeId, mEndpointId, app::Clusters::BasicInformation::Id, attributeId);
    };
    uint16_t vendorId  = PropertySetScalar<uint16_t>(readValue(VendorID::Id), L"VendorID");
    hstring vendorName = PropertySetScalar<hstring>(readValue(VendorName::Id), L"VendorName");
    uint16_t productId = PropertySetScalar<uint16_t>(readValue(ProductID::Id), L"ProductID");
    hstring productName = PropertySetScalar<hstring>(readValue(ProductName::Id), L"ProductName");
    hstring nodeLabel   = PropertySetScalar<hstring>(readValue(NodeLabel::Id), L"NodeLabel");
    hstring serialNumber;
    try
    {
        serialNumber = PropertySetScalar<hstring>(readValue(SerialNumber::Id), L"SerialNumber");
    }
    catch (hresult_error const & error)
    {
        if (to_string(error.message()).find("UNSUPPORTED_ATTRIBUTE") == std::string::npos)
        {
            throw;
        }
    }
    uint32_t softwareVersion = PropertySetScalar<uint32_t>(readValue(SoftwareVersion::Id), L"SoftwareVersion");
    hstring softwareVersionString =
        PropertySetScalar<hstring>(readValue(SoftwareVersionString::Id), L"SoftwareVersionString");
    co_return winrt::make<implementation::BasicInformation>(vendorId, vendorName, productId, productName, nodeLabel, serialNumber,
                                                            softwareVersion, softwareVersionString);
}

MatterController::MatterController(std::shared_ptr<ControllerRuntime> runtime) : mRuntime(std::move(runtime)) {}

std::shared_ptr<ControllerRuntime> MatterController::Runtime()
{
    std::scoped_lock lock(mRuntimeMutex);
    if (!mRuntime)
    {
        throw hresult_illegal_method_call(L"The Matter controller is closed.");
    }
    return mRuntime;
}

Windows::Foundation::IAsyncOperation<Controller::MatterController>
MatterController::CreateAsync(Controller::ControllerOptions options)
{
    co_await resume_background();
    co_return winrt::make<MatterController>(ControllerRuntime::Create(options));
}

event_token MatterController::CommissioningProgress(
    Windows::Foundation::TypedEventHandler<Controller::MatterController, Controller::CommissioningProgressEventArgs> const & handler)
{
    return mCommissioningProgress.add(handler);
}

void MatterController::CommissioningProgress(event_token const & token) noexcept
{
    mCommissioningProgress.remove(token);
}

Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
MatterController::CommissionOnNetworkAsync(Controller::OnNetworkCommissioningParameters parameters)
{
    if (!parameters)
    {
        throw hresult_invalid_argument(L"parameters cannot be null.");
    }
    auto progress = winrt::make<implementation::CommissioningProgressEventArgs>(Controller::CommissioningStage::Discovering,
                                                                                L"Starting on-network commissioning");
    mCommissioningProgress(*this, progress);
    auto runtime = Runtime();
    co_await resume_background();
    auto node = runtime->Commission(parameters.NodeId(), parameters.SetupPinCode(), parameters.LongDiscriminator(), false,
                                    to_string(parameters.SetupCode()));
    progress = winrt::make<implementation::CommissioningProgressEventArgs>(Controller::CommissioningStage::Complete,
                                                                           L"Commissioning complete");
    mCommissioningProgress(*this, progress);
    co_return node;
}

Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
MatterController::CommissionBleAsync(Controller::BleCommissioningParameters parameters)
{
    if (!parameters)
    {
        throw hresult_invalid_argument(L"parameters cannot be null.");
    }
    auto progress = winrt::make<implementation::CommissioningProgressEventArgs>(Controller::CommissioningStage::Discovering,
                                                                                L"Starting BLE commissioning");
    mCommissioningProgress(*this, progress);
    auto runtime = Runtime();
    co_await resume_background();
    auto node = runtime->Commission(parameters.NodeId(), parameters.SetupPinCode(), parameters.LongDiscriminator(), true);
    progress = winrt::make<implementation::CommissioningProgressEventArgs>(Controller::CommissioningStage::Complete,
                                                                           L"Commissioning complete");
    mCommissioningProgress(*this, progress);
    co_return node;
}

Windows::Foundation::Collections::IVectorView<Controller::CommissionedNode> MatterController::CommissionedNodes()
{
    auto runtime = Runtime();
    std::vector<Controller::CommissionedNode> nodes;
    for (uint64_t nodeId : runtime->CommissionedNodes())
    {
        nodes.push_back(winrt::make<implementation::CommissionedNode>(nodeId, runtime->FabricIndex()));
    }
    return single_threaded_vector(std::move(nodes)).GetView();
}

Windows::Foundation::IAsyncAction MatterController::RemoveNodeAsync(uint64_t nodeId)
{
    auto runtime = Runtime();
    co_await resume_background();
    runtime->RemoveNode(nodeId);
}

Windows::Foundation::IAsyncOperation<Controller::AttributeValue>
MatterController::ReadAttributeAsync(uint64_t nodeId, Controller::AttributePath path)
{
    if (!path)
    {
        throw hresult_invalid_argument(L"path cannot be null.");
    }
    auto runtime = Runtime();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled] { canceled->store(true, std::memory_order_release); });
    co_await resume_background();
    auto data = runtime->ReadAttribute(nodeId, path.EndpointId(), path.ClusterId(), path.AttributeId(), canceled);
    co_return winrt::make<implementation::AttributeValue>(path, data);
}

Windows::Foundation::IAsyncAction MatterController::WriteAttributeAsync(
    uint64_t nodeId, Controller::AttributePath path, Windows::Foundation::Collections::IPropertySet value)
{
    if (!path || !value)
    {
        throw hresult_invalid_argument(L"path and value cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    runtime->WriteAttribute(nodeId, path.EndpointId(), path.ClusterId(), path.AttributeId(), value);
}

Windows::Foundation::IAsyncAction MatterController::WriteAttributeTimedAsync(
    uint64_t nodeId, Controller::AttributePath path, Windows::Foundation::Collections::IPropertySet value,
    Controller::TimedInteractionOptions options)
{
    if (!path || !value || !options)
    {
        throw hresult_invalid_argument(L"path, value, and options cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    runtime->WriteAttribute(nodeId, path.EndpointId(), path.ClusterId(), path.AttributeId(), value,
                            options.TimeoutMilliseconds());
}

Windows::Foundation::IAsyncOperation<Controller::CommandResult> MatterController::InvokeCommandAsync(
    uint64_t nodeId, Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet arguments)
{
    if (!path || !arguments)
    {
        throw hresult_invalid_argument(L"path and arguments cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    auto data = runtime->InvokeCommand(nodeId, path.EndpointId(), path.ClusterId(), path.CommandId(), arguments);
    co_return winrt::make<implementation::CommandResult>(path, data);
}

Windows::Foundation::IAsyncOperation<Controller::CommandResult> MatterController::InvokeCommandTimedAsync(
    uint64_t nodeId, Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet arguments,
    Controller::TimedInteractionOptions options)
{
    if (!path || !arguments || !options)
    {
        throw hresult_invalid_argument(L"path, arguments, and options cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    auto data = runtime->InvokeCommand(nodeId, path.EndpointId(), path.ClusterId(), path.CommandId(), arguments,
                                       options.TimeoutMilliseconds());
    co_return winrt::make<implementation::CommandResult>(path, data);
}

Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription>
MatterController::SubscribeAttributeAsync(uint64_t nodeId, Controller::AttributePath path, uint16_t minimumIntervalSeconds,
                                          uint16_t maximumIntervalSeconds)
{
    if (!path)
    {
        throw hresult_invalid_argument(L"path cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    co_return runtime->SubscribeAttribute(nodeId, path.EndpointId(), path.ClusterId(), path.AttributeId(),
                                          minimumIntervalSeconds, maximumIntervalSeconds);
}

Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::EventValue>>
MatterController::ReadEventsAsync(uint64_t nodeId, Controller::EventPath path, uint64_t minimumEventNumber)
{
    if (!path)
    {
        throw hresult_invalid_argument(L"path cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    auto values = runtime->ReadEvents(nodeId, path.EndpointId(), path.ClusterId(), path.EventId(), path.Urgent(),
                                      minimumEventNumber);
    co_return single_threaded_vector(std::move(values)).GetView();
}

Windows::Foundation::IAsyncOperation<Controller::EventSubscription>
MatterController::SubscribeEventAsync(uint64_t nodeId, Controller::EventPath path, uint64_t minimumEventNumber,
                                      uint16_t minimumIntervalSeconds, uint16_t maximumIntervalSeconds)
{
    if (!path)
    {
        throw hresult_invalid_argument(L"path cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    co_return runtime->SubscribeEvent(nodeId, path.EndpointId(), path.ClusterId(), path.EventId(), path.Urgent(),
                                      minimumEventNumber, minimumIntervalSeconds, maximumIntervalSeconds);
}

Controller::OnOffCluster MatterController::GetOnOffCluster(uint64_t nodeId, uint16_t endpointId)
{
    return winrt::make<OnOffCluster>(Runtime(), nodeId, endpointId);
}

Controller::LevelControlCluster MatterController::GetLevelControlCluster(uint64_t nodeId, uint16_t endpointId)
{
    return winrt::make<LevelControlCluster>(Runtime(), nodeId, endpointId);
}

Controller::BasicInformationCluster MatterController::GetBasicInformationCluster(uint64_t nodeId, uint16_t endpointId)
{
    return winrt::make<BasicInformationCluster>(Runtime(), nodeId, endpointId);
}

Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
MatterControllerRecovery::RecoverNodeAsync(uint64_t nodeId)
{
    co_await resume_background();
    co_return mRuntime->RecoverNode(nodeId);
}

namespace {

constexpr size_t kMaximumDescriptorListItems = 512;
constexpr size_t kMaximumDiscoveredEndpoints = 128;

Windows::Foundation::Collections::IVector<Windows::Foundation::IInspectable>
DescriptorList(Windows::Foundation::Collections::IPropertySet const & data)
{
    if (!data || !data.HasKey(L"value") || !data.Lookup(L"value"))
    {
        throw hresult_error(E_BOUNDS, L"The Descriptor list is null or missing.");
    }
    auto list = data.Lookup(L"value").as<Windows::Foundation::Collections::IVector<Windows::Foundation::IInspectable>>();
    if (list.Size() > kMaximumDescriptorListItems)
    {
        throw hresult_error(E_BOUNDS, L"The Descriptor list exceeds the supported resource limit.");
    }
    return list;
}

template <typename Value>
Value DescriptorUnsigned(Windows::Foundation::IInspectable const & value)
{
    if (!value)
    {
        throw hresult_error(E_BOUNDS, L"A Descriptor numeric field is null.");
    }
    const uint64_t number = unbox_value<uint64_t>(value);
    if (number > std::numeric_limits<Value>::max())
    {
        throw hresult_error(E_BOUNDS, L"A Descriptor numeric field is outside its projected range.");
    }
    return static_cast<Value>(number);
}

template <typename Value>
std::vector<Value> DescriptorNumericList(Windows::Foundation::Collections::IPropertySet const & data)
{
    auto list = DescriptorList(data);
    std::vector<Value> result;
    result.reserve(list.Size());
    for (auto const & value : list)
    {
        result.push_back(DescriptorUnsigned<Value>(value));
    }
    return result;
}

Controller::MatterEndpointDescriptor ReadEndpointDescriptor(
    std::shared_ptr<ControllerRuntime> const & runtime, uint64_t nodeId, uint16_t endpointId,
    std::shared_ptr<std::atomic_bool> const & cancellation, std::chrono::steady_clock::time_point deadline)
{
    using namespace chip::app::Clusters::Descriptor;
    auto parts = DescriptorNumericList<uint16_t>(
        runtime->ReadAttribute(nodeId, endpointId, Id, Attributes::PartsList::Id, cancellation, deadline));
    auto servers = DescriptorNumericList<uint32_t>(
        runtime->ReadAttribute(nodeId, endpointId, Id, Attributes::ServerList::Id, cancellation, deadline));
    auto clients = DescriptorNumericList<uint32_t>(
        runtime->ReadAttribute(nodeId, endpointId, Id, Attributes::ClientList::Id, cancellation, deadline));
    auto list = DescriptorList(
        runtime->ReadAttribute(nodeId, endpointId, Id, Attributes::DeviceTypeList::Id, cancellation, deadline));
    std::vector<Controller::MatterDeviceType> types;
    types.reserve(list.Size());
    for (auto const & value : list)
    {
        auto fields = value.as<Windows::Foundation::Collections::IPropertySet>();
        if (!fields.HasKey(L"0") || !fields.HasKey(L"1"))
        {
            throw hresult_error(E_BOUNDS, L"A Descriptor device-type entry is missing required fields.");
        }
        const auto deviceType = DescriptorUnsigned<uint32_t>(fields.Lookup(L"0"));
        const auto revision = DescriptorUnsigned<uint16_t>(fields.Lookup(L"1"));
        if (revision == 0)
        {
            throw hresult_error(E_BOUNDS, L"A Descriptor device-type revision cannot be zero.");
        }
        types.push_back(winrt::make<implementation::MatterDeviceType>(deviceType, revision));
    }
    return winrt::make<implementation::MatterEndpointDescriptor>(endpointId, std::move(parts), std::move(servers),
                                                                 std::move(clients), std::move(types));
}

template <typename Value>
Value CameraStreamField(Windows::Foundation::Collections::IPropertySet const & fields, wchar_t const * key)
{
    if (!fields || !fields.HasKey(key))
    {
        throw hresult_error(E_BOUNDS, L"Camera stream metadata is missing a required field.");
    }
    return DescriptorUnsigned<Value>(fields.Lookup(key));
}

Controller::MatterCameraClusterCapabilities ReadCameraClusterCapabilities(
    std::shared_ptr<ControllerRuntime> const & runtime, uint64_t nodeId, uint16_t endpointId, uint32_t cluster,
    std::shared_ptr<std::atomic_bool> const & canceled, std::chrono::steady_clock::time_point deadline)
{
    using namespace chip::app::Clusters::Globals;
    auto revision = runtime->ReadAttribute(nodeId, endpointId, cluster, Attributes::ClusterRevision::Id, canceled, deadline);
    auto features = runtime->ReadAttribute(nodeId, endpointId, cluster, Attributes::FeatureMap::Id, canceled, deadline);
    auto attributes = DescriptorNumericList<uint32_t>(
        runtime->ReadAttribute(nodeId, endpointId, cluster, Attributes::AttributeList::Id, canceled, deadline));
    auto accepted = DescriptorNumericList<uint32_t>(
        runtime->ReadAttribute(nodeId, endpointId, cluster, Attributes::AcceptedCommandList::Id, canceled, deadline));
    auto generated = DescriptorNumericList<uint32_t>(
        runtime->ReadAttribute(nodeId, endpointId, cluster, Attributes::GeneratedCommandList::Id, canceled, deadline));
    return winrt::make<implementation::MatterCameraClusterCapabilities>(cluster,
        DescriptorUnsigned<uint16_t>(revision.Lookup(L"value")), DescriptorUnsigned<uint32_t>(features.Lookup(L"value")),
        std::move(attributes), std::move(accepted), std::move(generated));
}

Controller::MatterCameraCapabilities ReadCameraCapabilities(
    std::shared_ptr<ControllerRuntime> const & runtime, uint64_t nodeId, Controller::MatterEndpointDescriptor const & descriptor,
    std::shared_ptr<std::atomic_bool> const & canceled, std::chrono::steady_clock::time_point deadline)
{
    if (!descriptor.HasCameraClusters())
    {
        throw hresult_not_implemented(L"The endpoint does not expose camera streaming and WebRTC provider clusters.");
    }
    auto stream = ReadCameraClusterCapabilities(runtime, nodeId, descriptor.EndpointId(),
        chip::app::Clusters::CameraAvStreamManagement::Id, canceled, deadline);
    auto transport = ReadCameraClusterCapabilities(runtime, nodeId, descriptor.EndpointId(),
        chip::app::Clusters::WebRTCTransportProvider::Id, canceled, deadline);
    return winrt::make<implementation::MatterCameraCapabilities>(descriptor, std::move(stream), std::move(transport));
}

bool CameraPrivacyEnabled(std::shared_ptr<ControllerRuntime> const & runtime, uint64_t nodeId, uint16_t endpointId,
    Windows::Foundation::Collections::IVectorView<uint32_t> const & attributes,
    std::shared_ptr<std::atomic_bool> const & canceled,
    std::chrono::steady_clock::time_point deadline)
{
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    for (auto attribute : { Attributes::HardPrivacyModeOn::Id, Attributes::SoftLivestreamPrivacyModeEnabled::Id })
    {
        if (std::find(attributes.begin(), attributes.end(), attribute) != attributes.end() &&
            PropertySetScalar<bool>(runtime->ReadAttribute(nodeId, endpointId, Id,
                                                           attribute, canceled, deadline), L"Camera privacy state"))
        {
            return true;
        }
    }
    return false;
}

void ReadCameraPrivacy(std::shared_ptr<ControllerRuntime> const & runtime, uint64_t nodeId,
    Controller::MatterCameraCapabilities const & capabilities, std::shared_ptr<std::atomic_bool> const & canceled,
    std::chrono::steady_clock::time_point deadline)
{
    if (CameraPrivacyEnabled(runtime, nodeId, capabilities.Descriptor().EndpointId(),
                             capabilities.StreamManagement().Attributes(), canceled, deadline))
    {
        throw hresult_error(E_ACCESSDENIED, L"Camera privacy mode prevents streaming.");
    }
}

template <class Request>
Windows::Foundation::Collections::IPropertySet CameraCommandArguments(Request const & request)
{
    uint8_t encoded[512];
    TLV::TLVWriter writer;
    writer.Init(encoded, sizeof(encoded));
    CheckChipError(request.Encode(writer, TLV::AnonymousTag()), L"Encode typed camera command");
    CheckChipError(writer.Finalize(), L"Finalize typed camera command");
    TLV::TLVReader reader;
    reader.Init(encoded, writer.GetLengthWritten());
    CheckChipError(reader.Next(), L"Read typed camera command");
    return DecodePropertySetRoot(reader);
}

void DeallocateCameraVideoStream(std::shared_ptr<ControllerRuntime> const & runtime, uint64_t nodeId,
                                 uint16_t endpointId, uint16_t streamId)
{
    if (!IsOperationalNodeId(nodeId) || endpointId == kInvalidEndpointId || streamId == UINT16_MAX)
    {
        throw hresult_invalid_argument(L"A valid camera endpoint and owned video stream identifier are required.");
    }
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    Commands::VideoStreamDeallocate::Type request;
    request.videoStreamID = streamId;
    runtime->InvokeCommand(nodeId, endpointId, Id, request.GetCommandId(), CameraCommandArguments(request),
                           std::nullopt, request.GetCommandId());
}

Windows::Foundation::Collections::IVectorView<Controller::MatterCameraVideoStream> ReadCameraVideoStreams(
    std::shared_ptr<ControllerRuntime> const & runtime, uint64_t nodeId, uint16_t endpointId,
    std::shared_ptr<std::atomic_bool> const & canceled, std::chrono::steady_clock::time_point deadline)
{
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    auto list = DescriptorList(runtime->ReadAttribute(nodeId, endpointId, Id, Attributes::AllocatedVideoStreams::Id,
                                                     canceled, deadline));
    std::vector<Controller::MatterCameraVideoStream> streams;
    streams.reserve(list.Size());
    for (auto const & entry : list)
    {
        auto stream = winrt::make<implementation::MatterCameraVideoStream>(
            entry.as<Windows::Foundation::Collections::IPropertySet>());
        if (std::any_of(streams.begin(), streams.end(), [&](auto const & existing) {
                return existing.StreamId() == stream.StreamId();
            }))
        {
            throw hresult_error(E_BOUNDS, L"Allocated video streams contain a duplicate identifier.");
        }
        streams.push_back(std::move(stream));
    }
    return single_threaded_vector(std::move(streams)).GetView();
}

Controller::MatterCameraVideoStream FindAllocatedLiveViewStream(
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraVideoStream> const & streams, uint16_t streamId)
{
    for (auto const & stream : streams)
    {
        // ReferenceCount measures transport use, not whether this stream is allocated.
        if (stream.StreamId() == streamId && stream.IsLiveView())
        {
            return stream;
        }
    }
    return nullptr;
}

} // namespace

MatterCameraStreamSelection::MatterCameraStreamSelection(Controller::MatterCameraStreamSelectionKind kind,
    Windows::Foundation::IReference<uint16_t> id) : mKind(kind), mId(id)
{
    using Kind = Controller::MatterCameraStreamSelectionKind;
    if ((kind != Kind::Absent && kind != Kind::AutoSelect && kind != Kind::ExplicitId) ||
        (kind == Kind::ExplicitId ? (!id || id.Value() == UINT16_MAX) : bool(id)))
    {
        throw hresult_invalid_argument(L"Only ExplicitId requires a nonreserved stream ID; Absent and AutoSelect require null.");
    }
}

MatterCameraAudioStreamOptions::MatterCameraAudioStreamOptions()
{
    mRequest.streamUsage = chip::app::Clusters::Globals::StreamUsageEnum::kLiveView;
    mRequest.audioCodec = chip::app::Clusters::CameraAvStreamManagement::AudioCodecEnum::kUnknownEnumValue;
}

chip::app::Clusters::CameraAvStreamManagement::Commands::AudioStreamAllocate::Type
MatterCameraAudioStreamOptions::Snapshot() const
{
    using Codec = chip::app::Clusters::CameraAvStreamManagement::AudioCodecEnum;
    if ((mRequest.audioCodec != Codec::kOpus && mRequest.audioCodec != Codec::kAacLc) ||
        mRequest.channelCount == 0 || mRequest.channelCount > 8 || mRequest.sampleRate == 0 || mRequest.bitRate == 0 ||
        (mRequest.bitDepth != 8 && mRequest.bitDepth != 16 && mRequest.bitDepth != 24 && mRequest.bitDepth != 32))
    {
        throw hresult_invalid_argument(L"A negotiated known codec, 1-8 channels, nonzero sample rate/bitrate, and 8/16/24/32 bit depth are required.");
    }
    return mRequest;
}

void MatterCameraAudioStreamOptions::ValidateAllocated(
    chip::app::Clusters::CameraAvStreamManagement::Commands::AudioStreamAllocate::Type const & request,
    Controller::MatterCameraAudioStream const & stream)
{
    if (!stream || !stream.IsLiveView() || stream.AudioCodec() != static_cast<uint8_t>(request.audioCodec) ||
        stream.Channels() != request.channelCount || stream.SampleRate() != request.sampleRate ||
        stream.BitRate() != request.bitRate || stream.BitDepth() != request.bitDepth)
    {
        throw hresult_error(E_BOUNDS, L"Allocated audio metadata does not match the request.");
    }
}

MatterCameraAudioStream::MatterCameraAudioStream(Windows::Foundation::Collections::IPropertySet const & fields)
{
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    mStream.audioStreamID = CameraStreamField<uint16_t>(fields, L"0");
    mStream.streamUsage = static_cast<chip::app::Clusters::Globals::StreamUsageEnum>(CameraStreamField<uint8_t>(fields, L"1"));
    mStream.audioCodec = static_cast<AudioCodecEnum>(CameraStreamField<uint8_t>(fields, L"2"));
    mStream.channelCount = CameraStreamField<uint8_t>(fields, L"3");
    mStream.sampleRate = CameraStreamField<uint32_t>(fields, L"4");
    mStream.bitRate = CameraStreamField<uint32_t>(fields, L"5");
    mStream.bitDepth = CameraStreamField<uint8_t>(fields, L"6");
    mStream.referenceCount = CameraStreamField<uint8_t>(fields, L"7");
    if (mStream.audioStreamID == UINT16_MAX || mStream.channelCount == 0 || mStream.channelCount > 8 ||
        mStream.sampleRate == 0 || mStream.bitRate == 0 || mStream.bitDepth == 0)
    {
        throw hresult_error(E_BOUNDS, L"Allocated audio has invalid identifiers or format bounds.");
    }
}

MatterCameraAudioFormatCapabilities::MatterCameraAudioFormatCapabilities(
    Windows::Foundation::Collections::IPropertySet const & fields) : mChannels(CameraStreamField<uint8_t>(fields, L"0"))
{
    auto list = [&](wchar_t const * key) {
        if (!fields.HasKey(key))
        {
            throw hresult_error(E_BOUNDS, L"Audio capabilities are missing a required list.");
        }
        Windows::Foundation::Collections::PropertySet root;
        root.Insert(L"value", fields.Lookup(key));
        return root;
    };
    mCodecs = single_threaded_vector(DescriptorNumericList<uint8_t>(list(L"1"))).GetView();
    mRates = single_threaded_vector(DescriptorNumericList<uint32_t>(list(L"2"))).GetView();
    mDepths = single_threaded_vector(DescriptorNumericList<uint8_t>(list(L"3"))).GetView();
    // Preserve advertised values, including unusable entries, for caller diagnostics.
    // Request validation never substitutes a format for a malformed entry.
}

void MatterCameraAudioFormatCapabilities::Validate(
    chip::app::Clusters::CameraAvStreamManagement::Commands::AudioStreamAllocate::Type const & request) const
{
    if (mChannels == 0 || mChannels > 8 || request.channelCount == 0 || request.channelCount > mChannels ||
        request.sampleRate == 0 || request.bitDepth == 0 ||
        std::find(mCodecs.begin(), mCodecs.end(), static_cast<uint8_t>(request.audioCodec)) == mCodecs.end() ||
        std::find(mRates.begin(), mRates.end(), request.sampleRate) == mRates.end() ||
        std::find(mDepths.begin(), mDepths.end(), request.bitDepth) == mDepths.end())
    {
        throw hresult_not_implemented(L"The requested audio format is not advertised by the camera microphone.");
    }
}

MatterCameraVideoStreamOptions::MatterCameraVideoStreamOptions()
{
    mRequest.streamUsage = chip::app::Clusters::Globals::StreamUsageEnum::kLiveView;
    mRequest.videoCodec = chip::app::Clusters::CameraAvStreamManagement::VideoCodecEnum::kH264;
    mRequest.minResolution.width = mRequest.maxResolution.width = 640;
    mRequest.minResolution.height = mRequest.maxResolution.height = 480;
    mRequest.minFrameRate = mRequest.maxFrameRate = 30;
    mRequest.minBitRate = 500000;
    mRequest.maxBitRate = 2000000;
    mRequest.keyFrameInterval = 1000;
}

chip::app::Clusters::CameraAvStreamManagement::Commands::VideoStreamAllocate::Type
MatterCameraVideoStreamOptions::Snapshot() const
{
    if (mRequest.minResolution.width == 0 || mRequest.minResolution.height == 0 ||
        mRequest.minFrameRate == 0 || mRequest.minBitRate == 0 || mRequest.keyFrameInterval == 0 ||
        mRequest.minResolution.width > mRequest.maxResolution.width ||
        mRequest.minResolution.height > mRequest.maxResolution.height ||
        mRequest.minFrameRate > mRequest.maxFrameRate || mRequest.minBitRate > mRequest.maxBitRate)
    {
        throw hresult_invalid_argument(L"Video stream bounds must be nonzero and ordered.");
    }
    return mRequest;
}

void MatterCameraVideoStreamOptions::ValidateAllocated(
    chip::app::Clusters::CameraAvStreamManagement::Commands::VideoStreamAllocate::Type const & request,
    Controller::MatterCameraVideoStream const & stream)
{
    if (!stream || !stream.IsLiveView() || stream.VideoCodec() != static_cast<uint8_t>(request.videoCodec) ||
        stream.MinimumWidth() < request.minResolution.width ||
        stream.MinimumHeight() < request.minResolution.height || stream.MaximumWidth() > request.maxResolution.width ||
        stream.MaximumHeight() > request.maxResolution.height || stream.MinimumFrameRate() < request.minFrameRate ||
        stream.MaximumFrameRate() > request.maxFrameRate || stream.MinimumBitRate() < request.minBitRate ||
        stream.MaximumBitRate() > request.maxBitRate || stream.KeyFrameInterval() != request.keyFrameInterval)
    {
        throw hresult_error(E_BOUNDS, L"Allocated video metadata does not match the request.");
    }
}

MatterCameraVideoStream::MatterCameraVideoStream(Windows::Foundation::Collections::IPropertySet const & fields) :
    mStreamId(CameraStreamField<uint16_t>(fields, L"0")),
    mStreamUsage(CameraStreamField<uint8_t>(fields, L"1")),
    mVideoCodec(CameraStreamField<uint8_t>(fields, L"2")),
    mMinimumFrameRate(CameraStreamField<uint16_t>(fields, L"3")),
    mMaximumFrameRate(CameraStreamField<uint16_t>(fields, L"4")),
    mMinimumBitRate(CameraStreamField<uint32_t>(fields, L"7")),
    mMaximumBitRate(CameraStreamField<uint32_t>(fields, L"8")),
    mKeyFrameInterval(CameraStreamField<uint16_t>(fields, L"9")),
    mReferenceCount(CameraStreamField<uint8_t>(fields, L"12"))
{
    if (!fields.HasKey(L"5") || !fields.HasKey(L"6"))
    {
        throw hresult_error(E_BOUNDS, L"An allocated video stream is missing its resolution bounds.");
    }
    auto minimum = fields.Lookup(L"5").as<Windows::Foundation::Collections::IPropertySet>();
    auto maximum = fields.Lookup(L"6").as<Windows::Foundation::Collections::IPropertySet>();
    mMinimumWidth = CameraStreamField<uint16_t>(minimum, L"0");
    mMinimumHeight = CameraStreamField<uint16_t>(minimum, L"1");
    mMaximumWidth = CameraStreamField<uint16_t>(maximum, L"0");
    mMaximumHeight = CameraStreamField<uint16_t>(maximum, L"1");
    if (mStreamId == UINT16_MAX || mMinimumFrameRate > mMaximumFrameRate ||
        mMinimumWidth > mMaximumWidth || mMinimumHeight > mMaximumHeight || mMinimumBitRate > mMaximumBitRate)
    {
        throw hresult_error(E_BOUNDS, L"An allocated video stream has invalid identifiers or reversed bounds.");
    }
    if (fields.HasKey(L"10"))
    {
        mWatermark = box_value(unbox_value<bool>(fields.Lookup(L"10"))).as<Windows::Foundation::IReference<bool>>();
    }
    if (fields.HasKey(L"11"))
    {
        mOnScreenDisplay = box_value(unbox_value<bool>(fields.Lookup(L"11"))).as<Windows::Foundation::IReference<bool>>();
    }
}

bool MatterCameraVideoStream::IsLiveView() const
{
    return mStreamUsage == static_cast<uint8_t>(chip::app::Clusters::Globals::StreamUsageEnum::kLiveView);
}

MatterCameraIceCandidate::MatterCameraIceCandidate(hstring candidate, hstring mid, int32_t lineIndex) :
    mCandidate(std::move(candidate)), mMid(std::move(mid)), mLineIndex(lineIndex)
{
    const auto text = to_string(mCandidate);
    const auto encodedMid = to_string(mMid);
    if (text.empty() || text.size() > 2048 || text.find('\0') != std::string::npos ||
        encodedMid.size() > 256 || encodedMid.find('\0') != std::string::npos || lineIndex < -1 || lineIndex >= UINT16_MAX)
    {
        throw hresult_invalid_argument(L"ICE candidate fields exceed supported bounds.");
    }
}

MatterCameraIceServer::MatterCameraIceServer(Windows::Foundation::Collections::IVectorView<hstring> urls,
    bool hasUsername, hstring username, bool hasCredential, hstring credential,
    Windows::Foundation::IReference<uint16_t> certificateAuthorityId) :
    mHasUsername(hasUsername), mUsername(std::move(username)), mHasCredential(hasCredential),
    mCredential(std::move(credential)), mCertificateAuthorityId(certificateAuthorityId)
{
    if (!urls || urls.Size() == 0 || urls.Size() > 4 ||
        (!hasUsername && !mUsername.empty()) || (!hasCredential && !mCredential.empty()))
    {
        throw hresult_invalid_argument(L"ICE server URL count or optional-field presence is invalid.");
    }
    std::vector<hstring> copiedUrls;
    for (auto const & url : urls)
    {
        auto encoded = to_string(url);
        const auto prefix = encoded.find(':');
        if (encoded.size() > 512 || prefix == std::string::npos || prefix + 1 == encoded.size() ||
            (encoded.substr(0, prefix) != "stun" && encoded.substr(0, prefix) != "stuns" &&
             encoded.substr(0, prefix) != "turn" && encoded.substr(0, prefix) != "turns") ||
            std::any_of(encoded.begin(), encoded.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
        {
            throw hresult_invalid_argument(L"ICE server URLs must be bounded STUN or TURN URIs without whitespace or controls.");
        }
        copiedUrls.push_back(url);
    }
    for (auto const & field : { mUsername, mCredential })
    {
        auto encoded = to_string(field);
        if (encoded.size() > 256 || encoded.find('\0') != std::string::npos)
        {
            throw hresult_invalid_argument(L"ICE server credentials exceed supported bounds.");
        }
    }
    mUrls = single_threaded_vector(std::move(copiedUrls)).GetView();
}

MatterCameraSignal::MatterCameraSignal(uint64_t handle, Controller::MatterCameraSignalKind kind, hstring sdp,
                                       std::vector<Controller::MatterCameraIceCandidate> candidates, uint32_t error,
                                       std::optional<uint16_t> protocolId, uint8_t reason) :
    mHandle(handle), mKind(kind), mSdp(std::move(sdp)), mCandidates(single_threaded_vector(std::move(candidates)).GetView()),
    mError(error), mReason(reason)
{
    if (protocolId.has_value())
    {
        mProtocolId = box_value(*protocolId).as<Windows::Foundation::IReference<uint16_t>>();
    }
}

MatterCameraSession::MatterCameraSession(std::shared_ptr<ControllerRuntime> runtime, uint64_t handle, uint64_t node,
                                         uint16_t endpoint, uint16_t requestor) :
    mRuntime(std::move(runtime)), mHandle(handle), mNode(node), mEndpoint(endpoint), mRequestor(requestor)
{}

void MatterCameraSession::SetMediaMetadata(Controller::MatterCameraStreamSelection const & video,
    Controller::MatterCameraStreamSelection const & audio, std::optional<uint16_t> videoId,
    std::optional<uint16_t> audioId, std::optional<uint16_t> protocolId)
{
    mVideo = video;
    mAudio = audio;
    auto boxed = [](std::optional<uint16_t> id) -> Windows::Foundation::IReference<uint16_t> {
        return id ? box_value(*id).as<Windows::Foundation::IReference<uint16_t>>() : nullptr;
    };
    mVideoId = boxed(videoId);
    mAudioId = boxed(audioId);
    mProtocolId = boxed(protocolId);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraSignal>
MatterCameraSession::WaitForSignalAsync(uint32_t timeoutMilliseconds)
{
    auto lifetime = get_strong();
    if (timeoutMilliseconds == 0 || timeoutMilliseconds > 30000)
    {
        throw hresult_invalid_argument(L"The signal timeout must be between 1 and 30000 milliseconds.");
    }
    if (mWaiting.exchange(true))
    {
        throw hresult_illegal_method_call(L"Only one signal consumer is allowed per session.");
    }
    struct WaitingGuard
    {
        std::atomic_bool & waiting;
        ~WaitingGuard() { waiting.store(false); }
    } waitingGuard{ mWaiting };
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled]() { canceled->store(true, std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    co_await resume_background();
    while (true)
    {
        if (canceled->load(std::memory_order_acquire))
        {
            throw hresult_canceled();
        }
        auto signal = mRuntime->PollCameraSignal(mHandle);
        if (signal)
        {
            co_return signal;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Camera signal wait timed out.");
        }
        co_await resume_after(std::chrono::milliseconds(10));
    }
}

namespace {

std::vector<Controller::MatterCameraIceCandidate> CameraCandidateValues(
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceCandidate> candidates)
{
    if (!candidates || candidates.Size() == 0 || candidates.Size() > 16)
    {
        throw hresult_invalid_argument(L"Between 1 and 16 ICE candidates are required.");
    }
    std::vector<Controller::MatterCameraIceCandidate> values;
    for (auto const & candidate : candidates)
    {
        if (!candidate)
        {
            throw hresult_invalid_argument(L"ICE candidates cannot contain null entries.");
        }
        values.push_back(candidate);
    }
    return values;
}

template <class Operation>
Controller::MatterCameraOperationResult RunCameraOperation(Controller::MatterCameraStage & stage, Operation operation)
{
    try
    {
        operation();
        return winrt::make<implementation::MatterCameraOperationResult>();
    }
    catch (hresult_canceled const &)
    {
        throw;
    }
    catch (ChipOperationError const & error)
    {
        return winrt::make<implementation::MatterCameraOperationResult>(
            winrt::make<implementation::MatterCameraFailure>(stage, int32_t(error.code()), true,
                error.NativeError().AsInteger(), error.message()));
    }
    catch (hresult_error const & error)
    {
        return winrt::make<implementation::MatterCameraOperationResult>(
            winrt::make<implementation::MatterCameraFailure>(stage, int32_t(error.code()), false, uint32_t(0), error.message()));
    }
}

} // namespace

Windows::Foundation::IAsyncAction MatterCameraSession::SendCandidatesAsync(
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceCandidate> candidates)
{
    auto lifetime = get_strong();
    auto values = CameraCandidateValues(candidates);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled]() { canceled->store(true, std::memory_order_release); });
    co_await resume_background();
    mRuntime->SendCameraCandidates(mHandle, values, canceled);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraOperationResult> MatterCameraSession::TrySendCandidatesAsync(
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceCandidate> candidates)
{
    auto lifetime = get_strong();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled]() { canceled->store(true, std::memory_order_release); });
    auto stage = Controller::MatterCameraStage::Validation;
    std::vector<Controller::MatterCameraIceCandidate> values;
    auto validation = RunCameraOperation(stage, [&]() { values = CameraCandidateValues(candidates); });
    if (!validation.Succeeded())
    {
        co_return validation;
    }
    co_await resume_background();
    stage = Controller::MatterCameraStage::IceCandidates;
    co_return RunCameraOperation(stage, [&]() {
        if (canceled->load(std::memory_order_acquire))
        {
            throw hresult_canceled();
        }
        mRuntime->SendCameraCandidates(mHandle, values, canceled);
    });
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraSessionState> MatterCameraSession::GetStateAsync()
{
    auto lifetime = get_strong();
    if (mClosing.load(std::memory_order_acquire) != 0)
    {
        co_return Controller::MatterCameraSessionState::Ending;
    }
    co_await resume_background();
    co_return mRuntime->CameraState(mHandle);
}

namespace {

struct CameraCloseGuard
{
    explicit CameraCloseGuard(std::atomic_uint32_t & closing) : mClosing(closing) { ++mClosing; }
    ~CameraCloseGuard() { --mClosing; }
    std::atomic_uint32_t & mClosing;
};

} // namespace

Windows::Foundation::IAsyncAction MatterCameraSession::CloseAsync()
{
    auto lifetime = get_strong();
    CameraCloseGuard closing(mClosing);
    co_await resume_background();
    mRuntime->CloseCamera(mHandle);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraOperationResult> MatterCameraSession::TryCloseAsync()
{
    auto lifetime = get_strong();
    CameraCloseGuard closing(mClosing);
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::SessionEnd;
    // Cleanup must finish even when the caller cancels its wait.
    co_return RunCameraOperation(stage, [&]() { mRuntime->CloseCamera(mHandle); });
}

MatterControllerCamera::MatterControllerCamera(Controller::MatterController controller)
{
    if (!controller)
    {
        throw hresult_invalid_argument(L"controller cannot be null.");
    }
    mRuntime = get_self<implementation::MatterController>(controller)->Runtime();
}

bool MatterControllerCamera::IsSignalingSupported()
{
    return CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING != 0;
}

MatterCameraClusterCapabilities::MatterCameraClusterCapabilities(uint32_t cluster, uint16_t revision, uint32_t features,
    std::vector<uint32_t> attributes, std::vector<uint32_t> accepted, std::vector<uint32_t> generated) :
    mCluster(cluster), mRevision(revision), mFeatures(features),
    mAttributes(single_threaded_vector(std::move(attributes)).GetView()),
    mAccepted(single_threaded_vector(std::move(accepted)).GetView()),
    mGenerated(single_threaded_vector(std::move(generated)).GetView())
{
    if (revision == 0)
    {
        throw hresult_error(E_BOUNDS, L"A camera cluster revision cannot be zero.");
    }
}

bool MatterCameraCapabilities::SupportsVideo() const
{
    return (mStream.FeatureMap() & uint32_t(chip::app::Clusters::CameraAvStreamManagement::Feature::kVideo)) != 0;
}

bool MatterCameraCapabilities::SupportsClientOffer() const
{
    namespace Commands = chip::app::Clusters::WebRTCTransportProvider::Commands;
    auto accepted = mTransport.AcceptedCommands();
    auto generated = mTransport.GeneratedCommands();
    return SupportsVideo() &&
        std::find(accepted.begin(), accepted.end(), Commands::ProvideOffer::Id) != accepted.end() &&
        std::find(accepted.begin(), accepted.end(), Commands::ProvideICECandidates::Id) != accepted.end() &&
        std::find(accepted.begin(), accepted.end(), Commands::EndSession::Id) != accepted.end() &&
        std::find(generated.begin(), generated.end(), Commands::ProvideOfferResponse::Id) != generated.end();
}

namespace {

template <typename Range, typename Value>
bool CameraAdvertises(Range const & values, Value value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool IsCameraAudioAdapter(Controller::MatterCameraCapabilities const & capabilities)
{
    if (!capabilities)
    {
        return false;
    }
    const auto descriptor = capabilities.Descriptor();
    const auto types = descriptor.DeviceTypes();
    return std::any_of(types.begin(), types.end(), [](auto const & type) {
            return type.DeviceTypeId() == 0x0142 && type.Revision() <= 2;
        }) && descriptor.HasCameraClusters() &&
        CameraAdvertises(descriptor.ClientClusters(), chip::app::Clusters::WebRTCTransportRequestor::Id) &&
        capabilities.StreamManagement().Revision() == 2 && capabilities.TransportProvider().Revision() == 2;
}

void RequireCameraAudioAdapter(Controller::MatterCameraCapabilities const & capabilities)
{
    if (!IsCameraAudioAdapter(capabilities))
    {
        throw hresult_not_implemented(L"The audio adapter requires Camera device type revision 1-2, provider/requestor composition, and generated cluster revision 2.");
    }
}

bool CameraOffersAudio(Controller::MatterCameraCapabilities const & camera)
{
    using namespace chip::app::Clusters;
    auto stream = camera.StreamManagement();
    auto provider = camera.TransportProvider();
    return (stream.FeatureMap() & uint32_t(CameraAvStreamManagement::Feature::kAudio)) != 0 &&
        CameraAdvertises(stream.Attributes(), CameraAvStreamManagement::Attributes::MicrophoneCapabilities::Id) &&
        CameraAdvertises(stream.Attributes(), CameraAvStreamManagement::Attributes::AllocatedAudioStreams::Id) &&
        CameraAdvertises(stream.Attributes(), CameraAvStreamManagement::Attributes::SupportedStreamUsages::Id) &&
        CameraAdvertises(stream.AcceptedCommands(), CameraAvStreamManagement::Commands::AudioStreamAllocate::Id) &&
        CameraAdvertises(stream.AcceptedCommands(), CameraAvStreamManagement::Commands::AudioStreamDeallocate::Id) &&
        CameraAdvertises(stream.GeneratedCommands(), CameraAvStreamManagement::Commands::AudioStreamAllocateResponse::Id) &&
        CameraAdvertises(provider.AcceptedCommands(), WebRTCTransportProvider::Commands::ProvideOffer::Id) &&
        CameraAdvertises(provider.AcceptedCommands(), WebRTCTransportProvider::Commands::ProvideICECandidates::Id) &&
        CameraAdvertises(provider.AcceptedCommands(), WebRTCTransportProvider::Commands::EndSession::Id) &&
        CameraAdvertises(provider.GeneratedCommands(), WebRTCTransportProvider::Commands::ProvideOfferResponse::Id);
}

Controller::MatterCameraAudioCapabilities ReadCameraAudioCapabilities(
    std::shared_ptr<ControllerRuntime> const & runtime, uint64_t node, uint16_t endpoint,
    std::shared_ptr<std::atomic_bool> const & canceled, std::chrono::steady_clock::time_point deadline,
    Controller::MatterCameraStage & stage)
{
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    stage = Controller::MatterCameraStage::Validation;
    if (!IsOperationalNodeId(node) || endpoint == kInvalidEndpointId)
    {
        throw hresult_invalid_argument(L"A valid operational node and camera endpoint are required.");
    }
    stage = Controller::MatterCameraStage::Descriptor;
    auto descriptor = ReadEndpointDescriptor(runtime, node, endpoint, canceled, deadline);
    stage = Controller::MatterCameraStage::Capabilities;
    auto camera = ReadCameraCapabilities(runtime, node, descriptor, canceled, deadline);
    RequireCameraAudioAdapter(camera);
    auto result = winrt::make_self<implementation::MatterCameraAudioCapabilities>();
    result->mCamera = camera;
    const auto attributes = camera.StreamManagement().Attributes();
    const auto features = camera.StreamManagement().FeatureMap();
    auto read = [&](uint32_t attribute) { return runtime->ReadAttribute(node, endpoint, Id, attribute, canceled, deadline); };
    if ((features & uint32_t(Feature::kAudio)) != 0 && CameraAdvertises(attributes, Attributes::MicrophoneCapabilities::Id))
    {
        auto fields = read(Attributes::MicrophoneCapabilities::Id);
        try
        {
            result->mMicrophone = winrt::make<implementation::MatterCameraAudioFormatCapabilities>(fields);
        }
        catch (hresult_error const & error)
        {
            throw hresult_error(error.code(), L"MicrophoneCapabilities (attribute 7): " + error.message());
        }
    }
    if ((features & uint32_t(Feature::kSpeaker)) != 0 && CameraAdvertises(attributes, Attributes::SpeakerCapabilities::Id))
    {
        auto fields = read(Attributes::SpeakerCapabilities::Id);
        try
        {
            result->mSpeaker = winrt::make<implementation::MatterCameraAudioFormatCapabilities>(fields);
        }
        catch (hresult_error const & error)
        {
            throw hresult_error(error.code(), L"SpeakerCapabilities (attribute 8): " + error.message());
        }
    }
    auto number = [&](uint32_t attribute) -> Windows::Foundation::IReference<uint8_t> {
        if (!CameraAdvertises(attributes, attribute))
        {
            return nullptr;
        }
        return box_value(CameraStreamField<uint8_t>(read(attribute), L"value")).as<Windows::Foundation::IReference<uint8_t>>();
    };
    auto boolean = [&](uint32_t attribute) -> Windows::Foundation::IReference<bool> {
        if (!CameraAdvertises(attributes, attribute))
        {
            return nullptr;
        }
        return box_value(PropertySetScalar<bool>(read(attribute), L"Camera audio mute")).as<Windows::Foundation::IReference<bool>>();
    };
    if (result->mMicrophone)
    {
        result->mMicrophoneMuted = boolean(Attributes::MicrophoneMuted::Id);
        result->mMicrophoneVolume = number(Attributes::MicrophoneVolumeLevel::Id);
        result->mMicrophoneMin = number(Attributes::MicrophoneMinLevel::Id);
        result->mMicrophoneMax = number(Attributes::MicrophoneMaxLevel::Id);
    }
    if (result->mSpeaker)
    {
        result->mTalk = number(Attributes::TwoWayTalkSupport::Id);
        result->mSpeakerMuted = boolean(Attributes::SpeakerMuted::Id);
        result->mSpeakerVolume = number(Attributes::SpeakerVolumeLevel::Id);
        result->mSpeakerMin = number(Attributes::SpeakerMinLevel::Id);
        result->mSpeakerMax = number(Attributes::SpeakerMaxLevel::Id);
    }
    if (!CameraAdvertises(attributes, Attributes::MaxNetworkBandwidth::Id))
    {
        throw hresult_not_implemented(L"The camera does not advertise its network bandwidth constraint.");
    }
    result->mBandwidth = CameraStreamField<uint32_t>(read(Attributes::MaxNetworkBandwidth::Id), L"value");
    if (result->mBandwidth == 0)
    {
        throw hresult_error(E_BOUNDS, L"The camera advertises zero network bandwidth.");
    }
    stage = Controller::MatterCameraStage::Privacy;
    result->mPrivacy = CameraPrivacyEnabled(runtime, node, endpoint, attributes, canceled, deadline);
    return *result;
}

Windows::Foundation::Collections::IVectorView<Controller::MatterCameraAudioStream> ReadCameraAudioStreams(
    std::shared_ptr<ControllerRuntime> const & runtime, uint64_t node, uint16_t endpoint,
    std::shared_ptr<std::atomic_bool> const & canceled, std::chrono::steady_clock::time_point deadline)
{
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    auto list = DescriptorList(runtime->ReadAttribute(node, endpoint, Id, Attributes::AllocatedAudioStreams::Id, canceled, deadline));
    std::vector<Controller::MatterCameraAudioStream> streams;
    for (auto const & value : list)
    {
        auto stream = winrt::make<implementation::MatterCameraAudioStream>(value.as<Windows::Foundation::Collections::IPropertySet>());
        if (std::any_of(streams.begin(), streams.end(), [&](auto const & existing) { return existing.StreamId() == stream.StreamId(); }))
        {
            throw hresult_error(E_BOUNDS, L"Allocated audio streams contain a duplicate identifier.");
        }
        streams.push_back(std::move(stream));
    }
    return single_threaded_vector(std::move(streams)).GetView();
}

void RequireListening(Controller::MatterCameraAudioCapabilities const & capabilities, Controller::MatterCameraStage & stage)
{
    stage = Controller::MatterCameraStage::Capabilities;
    if (!capabilities.SupportsListening())
    {
        throw hresult_not_implemented(L"The camera does not advertise compatible microphone formats and the complete audio command path.");
    }
    stage = Controller::MatterCameraStage::Privacy;
    if (capabilities.PrivacyEnabled())
    {
        throw hresult_error(E_ACCESSDENIED, L"Camera privacy mode prevents audio streaming.");
    }
}

void DeallocateCameraAudioStream(std::shared_ptr<ControllerRuntime> const & runtime, uint64_t node,
    uint16_t endpoint, uint16_t stream)
{
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    runtime->AudioOwnership().StartRelease(node, endpoint, stream);
    try
    {
        Commands::AudioStreamDeallocate::Type request;
        request.audioStreamID = stream;
        runtime->InvokeCommand(node, endpoint, Id, request.GetCommandId(), CameraCommandArguments(request),
                               std::nullopt, request.GetCommandId());
    }
    catch (...)
    {
        runtime->AudioOwnership().ReleaseFinished(node, endpoint, false);
        throw;
    }
    runtime->AudioOwnership().ReleaseFinished(node, endpoint, true);
}

Controller::MatterCameraAudioStream AllocateCameraAudioStream(
    std::shared_ptr<ControllerRuntime> const & runtime, uint64_t node, uint16_t endpoint,
    chip::app::Clusters::CameraAvStreamManagement::Commands::AudioStreamAllocate::Type const & request,
    std::shared_ptr<std::atomic_bool> const & canceled, Controller::MatterCameraStage & stage,
    std::shared_ptr<PendingCameraStream> const & ownership)
{
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    auto capabilities = ReadCameraAudioCapabilities(runtime, node, endpoint, canceled, deadline, stage);
    RequireListening(capabilities, stage);
    stage = Controller::MatterCameraStage::StreamSelection;
    get_self<implementation::MatterCameraAudioFormatCapabilities>(capabilities.Microphone())->Validate(request);
    auto usages = DescriptorNumericList<uint8_t>(
        runtime->ReadAttribute(node, endpoint, Id, Attributes::SupportedStreamUsages::Id, canceled, deadline));
    if (!CameraAdvertises(usages, static_cast<uint8_t>(request.streamUsage)) || request.bitRate > capabilities.MaximumNetworkBandwidth())
    {
        throw hresult_not_implemented(L"Requested audio exceeds the advertised stream usage or bandwidth.");
    }
    if (canceled->load(std::memory_order_acquire))
    {
        throw hresult_canceled();
    }
    stage = Controller::MatterCameraStage::Validation;
    runtime->AudioOwnership().Reserve(node, endpoint);
    try
    {
        auto response = runtime->InvokeCommand(node, endpoint, Id, request.GetCommandId(), CameraCommandArguments(request),
            std::nullopt, Commands::AudioStreamAllocateResponse::Id, true, &stage);
        const auto id = CameraStreamField<uint16_t>(response, L"0");
        runtime->AudioOwnership().Commit(node, endpoint, id);
        try
        {
            ownership->Record(id);
            if (canceled->load(std::memory_order_acquire))
            {
                throw hresult_canceled();
            }
            stage = Controller::MatterCameraStage::StreamSelection;
            const auto streams = ReadCameraAudioStreams(runtime, node, endpoint, canceled, deadline);
            auto found = std::find_if(streams.begin(), streams.end(), [&](auto const & stream) { return stream.StreamId() == id; });
            if (found == streams.end())
            {
                throw hresult_error(E_BOUNDS, L"The owned audio allocation is absent from readback.");
            }
            const auto stream = *found;
            MatterCameraAudioStreamOptions::ValidateAllocated(request, stream);
            if (canceled->load(std::memory_order_acquire))
            {
                throw hresult_canceled();
            }
            return stream;
        }
        catch (...)
        {
            const auto failure = std::current_exception();
            const auto failedStage = stage;
            stage = Controller::MatterCameraStage::StreamDeallocation;
            ownership->Release();
            stage = failedStage;
            std::rethrow_exception(failure);
        }
    }
    catch (ChipOperationError const & error)
    {
        runtime->AudioOwnership().AllocationFailed(node, endpoint,
            stage == Controller::MatterCameraStage::Validation || error.NativeError().IsIMStatus());
        throw;
    }
    catch (...)
    {
        runtime->AudioOwnership().AllocationFailed(node, endpoint, stage == Controller::MatterCameraStage::Validation);
        throw;
    }
}

} // namespace

bool MatterCameraAudioCapabilities::SupportsListening() const
{
    using Codec = chip::app::Clusters::CameraAvStreamManagement::AudioCodecEnum;
    return IsCameraAudioAdapter(mCamera) && mMicrophone && CameraOffersAudio(mCamera) &&
        mMicrophone.MaximumChannels() >= 1 && mMicrophone.MaximumChannels() <= 8 &&
        std::any_of(mMicrophone.SampleRates().begin(), mMicrophone.SampleRates().end(),
            [](uint32_t rate) { return rate != 0; }) &&
        (CameraAdvertises(mMicrophone.Codecs(), static_cast<uint8_t>(Codec::kOpus)) ||
         CameraAdvertises(mMicrophone.Codecs(), static_cast<uint8_t>(Codec::kAacLc))) &&
        std::any_of(mMicrophone.BitDepths().begin(), mMicrophone.BitDepths().end(),
            [](uint8_t depth) { return depth == 8 || depth == 16 || depth == 24 || depth == 32; });
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraAudioCapabilities>
MatterControllerCamera::ReadAudioCapabilitiesAsync(uint64_t nodeId, uint16_t endpointId)
{
    auto lifetime = get_strong();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled] { canceled->store(true, std::memory_order_release); });
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::Validation;
    co_return ReadCameraAudioCapabilities(mRuntime, nodeId, endpointId, canceled,
        std::chrono::steady_clock::now() + std::chrono::seconds(120), stage);
}

Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::MatterCameraAudioStream>>
MatterControllerCamera::ReadAllocatedAudioStreamsAsync(uint64_t nodeId, uint16_t endpointId)
{
    auto lifetime = get_strong();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled] { canceled->store(true, std::memory_order_release); });
    co_await resume_background();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    auto stage = Controller::MatterCameraStage::Validation;
    auto capabilities = ReadCameraAudioCapabilities(mRuntime, nodeId, endpointId, canceled, deadline, stage);
    if ((capabilities.Camera().StreamManagement().FeatureMap() &
         uint32_t(chip::app::Clusters::CameraAvStreamManagement::Feature::kAudio)) == 0 ||
        !CameraAdvertises(capabilities.Camera().StreamManagement().Attributes(),
                          chip::app::Clusters::CameraAvStreamManagement::Attributes::AllocatedAudioStreams::Id))
    {
        throw hresult_not_implemented(L"Allocated audio streams are not advertised.");
    }
    co_return ReadCameraAudioStreams(mRuntime, nodeId, endpointId, canceled, deadline);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraAudioStream>
MatterControllerCamera::AllocateAudioStreamAsync(uint64_t nodeId, uint16_t endpointId, Controller::MatterCameraAudioStreamOptions options)
{
    auto lifetime = get_strong();
    if (!options)
    {
        throw hresult_invalid_argument(L"Audio stream options cannot be null.");
    }
    auto request = get_self<implementation::MatterCameraAudioStreamOptions>(options)->Snapshot();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto ownership = std::make_shared<PendingCameraStream>([runtime = mRuntime, nodeId, endpointId](uint16_t stream) {
        DeallocateCameraAudioStream(runtime, nodeId, endpointId, stream);
    });
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled, ownership] {
        canceled->store(true, std::memory_order_release);
        ownership->Cancel();
    });
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::Validation;
    co_return AllocateCameraAudioStream(mRuntime, nodeId, endpointId, request, canceled, stage, ownership);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraAudioStreamAllocationResult>
MatterControllerCamera::TryAllocateAudioStreamAsync(uint64_t nodeId, uint16_t endpointId, Controller::MatterCameraAudioStreamOptions options)
{
    auto lifetime = get_strong();
    auto stage = Controller::MatterCameraStage::Validation;
    chip::app::Clusters::CameraAvStreamManagement::Commands::AudioStreamAllocate::Type request;
    auto validation = RunCameraOperation(stage, [&] {
        if (!options)
        {
            throw hresult_invalid_argument(L"Audio stream options cannot be null.");
        }
        request = get_self<implementation::MatterCameraAudioStreamOptions>(options)->Snapshot();
    });
    if (!validation.Succeeded())
    {
        co_return winrt::make<implementation::MatterCameraAudioStreamAllocationResult>(validation.Failure());
    }
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto ownership = std::make_shared<PendingCameraStream>([runtime = mRuntime, nodeId, endpointId](uint16_t stream) {
        DeallocateCameraAudioStream(runtime, nodeId, endpointId, stream);
    });
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled, ownership] {
        canceled->store(true, std::memory_order_release);
        ownership->Cancel();
    });
    co_await resume_background();
    Controller::MatterCameraAudioStreamAllocationResult allocation{ nullptr };
    auto result = RunCameraOperation(stage, [&] {
        auto stream = AllocateCameraAudioStream(mRuntime, nodeId, endpointId, request, canceled, stage, ownership);
        try
        {
            allocation = winrt::make<implementation::MatterCameraAudioStreamAllocationResult>(stream);
        }
        catch (...)
        {
            stage = Controller::MatterCameraStage::StreamDeallocation;
            ownership->Release();
            throw;
        }
    });
    co_return result.Succeeded() ? allocation : winrt::make<implementation::MatterCameraAudioStreamAllocationResult>(result.Failure());
}

Windows::Foundation::IAsyncAction MatterControllerCamera::DeallocateAudioStreamAsync(uint64_t nodeId, uint16_t endpointId, uint16_t streamId)
{
    auto lifetime = get_strong();
    co_await resume_background();
    DeallocateCameraAudioStream(mRuntime, nodeId, endpointId, streamId);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraOperationResult>
MatterControllerCamera::TryDeallocateAudioStreamAsync(uint64_t nodeId, uint16_t endpointId, uint16_t streamId)
{
    auto lifetime = get_strong();
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::StreamDeallocation;
    co_return RunCameraOperation(stage, [&] { DeallocateCameraAudioStream(mRuntime, nodeId, endpointId, streamId); });
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraCapabilities>
MatterControllerCamera::ReadCapabilitiesAsync(uint64_t nodeId, uint16_t endpointId)
{
    auto lifetime = get_strong();
    if (!IsOperationalNodeId(nodeId) || endpointId == kInvalidEndpointId)
    {
        throw hresult_invalid_argument(L"A valid operational node and camera endpoint are required.");
    }
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled]() { canceled->store(true, std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    co_await resume_background();
    auto descriptor = ReadEndpointDescriptor(mRuntime, nodeId, endpointId, canceled, deadline);
    co_return ReadCameraCapabilities(mRuntime, nodeId, descriptor, canceled, deadline);
}

Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::MatterCameraVideoStream>>
MatterControllerCamera::ReadAllocatedVideoStreamsAsync(uint64_t nodeId, uint16_t endpointId)
{
    auto lifetime = get_strong();
    if (!IsOperationalNodeId(nodeId) || endpointId == kInvalidEndpointId)
    {
        throw hresult_invalid_argument(L"A valid operational node and camera endpoint are required.");
    }
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled]() { canceled->store(true, std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    co_await resume_background();
    if (!ReadEndpointDescriptor(mRuntime, nodeId, endpointId, canceled, deadline).HasCameraClusters())
    {
        throw hresult_error(E_NOTIMPL, L"The endpoint does not expose camera streaming and WebRTC provider clusters.");
    }
    co_return ReadCameraVideoStreams(mRuntime, nodeId, endpointId, canceled, deadline);
}

namespace {

Controller::MatterCameraVideoStream AllocateCameraVideoStream(
    std::shared_ptr<ControllerRuntime> const & runtime, uint64_t nodeId, uint16_t endpointId,
    chip::app::Clusters::CameraAvStreamManagement::Commands::VideoStreamAllocate::Type const & request,
    std::shared_ptr<std::atomic_bool> const & canceled, Controller::MatterCameraStage & stage,
    std::shared_ptr<PendingCameraVideoStream> const & ownership)
{
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    stage = Controller::MatterCameraStage::Validation;
    if (!IsOperationalNodeId(nodeId) || endpointId == kInvalidEndpointId)
    {
        throw hresult_invalid_argument(L"A valid operational node and camera endpoint are required.");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    stage = Controller::MatterCameraStage::Descriptor;
    auto descriptor = ReadEndpointDescriptor(runtime, nodeId, endpointId, canceled, deadline);
    stage = Controller::MatterCameraStage::Capabilities;
    auto capabilities = ReadCameraCapabilities(runtime, nodeId, descriptor, canceled, deadline);
    auto commands = capabilities.StreamManagement().AcceptedCommands();
    auto responses = capabilities.StreamManagement().GeneratedCommands();
    if (!capabilities.SupportsVideo() ||
        std::find(commands.begin(), commands.end(), Commands::VideoStreamAllocate::Id) == commands.end() ||
        std::find(commands.begin(), commands.end(), Commands::VideoStreamDeallocate::Id) == commands.end() ||
        std::find(responses.begin(), responses.end(), Commands::VideoStreamAllocateResponse::Id) == responses.end())
    {
        throw hresult_not_implemented(L"The camera does not advertise video allocation, response and deallocation.");
    }
    stage = Controller::MatterCameraStage::Privacy;
    ReadCameraPrivacy(runtime, nodeId, capabilities, canceled, deadline);
    stage = Controller::MatterCameraStage::StreamSelection;
    auto sensor = runtime->ReadAttribute(nodeId, endpointId, Id, Attributes::VideoSensorParams::Id, canceled, deadline);
    auto viewport = runtime->ReadAttribute(nodeId, endpointId, Id, Attributes::MinViewportResolution::Id, canceled, deadline);
    auto usages = DescriptorNumericList<uint8_t>(
        runtime->ReadAttribute(nodeId, endpointId, Id, Attributes::SupportedStreamUsages::Id, canceled, deadline));
    if (std::find(usages.begin(), usages.end(), static_cast<uint8_t>(request.streamUsage)) == usages.end())
    {
        throw hresult_not_implemented(L"The camera does not advertise LiveView stream usage.");
    }
    if (request.maxResolution.width > CameraStreamField<uint16_t>(sensor, L"0") ||
        request.maxResolution.height > CameraStreamField<uint16_t>(sensor, L"1") ||
        request.maxFrameRate > CameraStreamField<uint16_t>(sensor, L"2") ||
        request.minResolution.width < CameraStreamField<uint16_t>(viewport, L"0") ||
        request.minResolution.height < CameraStreamField<uint16_t>(viewport, L"1"))
    {
        throw hresult_invalid_argument(L"Requested video exceeds camera sensor or viewport bounds.");
    }
    if (canceled->load(std::memory_order_acquire))
    {
        throw hresult_canceled();
    }
    stage = Controller::MatterCameraStage::Validation;
    // Do not cancel a dispatched allocation before recovering its owned reference.
    auto response = runtime->InvokeCommand(nodeId, endpointId, Id, request.GetCommandId(), CameraCommandArguments(request),
                                          std::nullopt, Commands::VideoStreamAllocateResponse::Id, true, &stage);
    const auto streamId = CameraStreamField<uint16_t>(response, L"0");
    if (streamId == UINT16_MAX)
    {
        throw hresult_error(E_BOUNDS, L"Allocation returned a reserved stream identifier; remote ownership is uncertain.");
    }
    ownership->Record(streamId);
    try
    {
        if (canceled->load(std::memory_order_acquire))
        {
            throw hresult_canceled();
        }
        stage = Controller::MatterCameraStage::StreamSelection;
        auto streams = ReadCameraVideoStreams(runtime, nodeId, endpointId, canceled, deadline);
        auto stream = FindAllocatedLiveViewStream(streams, streamId);
        if (!stream)
        {
            throw hresult_error(E_BOUNDS, L"The owned allocation is absent from the camera's allocated LiveView stream list.");
        }
        MatterCameraVideoStreamOptions::ValidateAllocated(request, stream);
        if (canceled->load(std::memory_order_acquire))
        {
            throw hresult_canceled();
        }
        return stream;
    }
    catch (...)
    {
        const auto failure = std::current_exception();
        const auto failedStage = stage;
        stage = Controller::MatterCameraStage::StreamDeallocation;
        ownership->Release();
        stage = failedStage;
        std::rethrow_exception(failure);
    }
}

Controller::MatterCameraSession StartPreparedCamera(
    std::shared_ptr<ControllerRuntime> const & runtime, uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId,
    std::string const & sdp, std::shared_ptr<std::atomic_bool> const & canceled, Controller::MatterCameraStage & stage,
    std::shared_ptr<PendingCameraSession> const & ownership,
    std::vector<Controller::MatterCameraIceServer> const & iceServers = {})
{
    stage = Controller::MatterCameraStage::Validation;
    if (canceled->load(std::memory_order_acquire))
    {
        throw hresult_canceled();
    }
    if (!IsOperationalNodeId(nodeId) || endpointId == kInvalidEndpointId || videoStreamId == UINT16_MAX ||
        sdp.empty() || sdp.size() > 16384 || sdp.find('\0') != std::string::npos)
    {
        throw hresult_invalid_argument(L"A valid camera, allocated video stream, and bounded SDP offer are required.");
    }
    runtime->EnsureCameraSignalingEnabled();
    stage = Controller::MatterCameraStage::Descriptor;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    auto descriptor = ReadEndpointDescriptor(runtime, nodeId, endpointId, canceled, deadline);
    if (!descriptor.HasCameraClusters())
    {
        throw hresult_error(E_NOTIMPL, L"The endpoint does not expose camera streaming and WebRTC provider clusters.");
    }
    stage = Controller::MatterCameraStage::Capabilities;
    auto capabilities = ReadCameraCapabilities(runtime, nodeId, descriptor, canceled, deadline);
    if (!capabilities.SupportsClientOffer())
    {
        throw hresult_not_implemented(L"The camera does not advertise video and the required client-offer commands.");
    }
    stage = Controller::MatterCameraStage::Privacy;
    ReadCameraPrivacy(runtime, nodeId, capabilities, canceled, deadline);
    stage = Controller::MatterCameraStage::StreamSelection;
    auto streams = ReadCameraVideoStreams(runtime, nodeId, endpointId, canceled, deadline);
    if (!FindAllocatedLiveViewStream(streams, videoStreamId))
    {
        throw hresult_error(E_INVALIDARG, L"The selected video stream is not an allocated LiveView stream.");
    }
    stage = Controller::MatterCameraStage::Signaling;
    auto session = runtime->StartCamera(nodeId, endpointId, videoStreamId, sdp, canceled, iceServers);
    ownership->Record(session.Handle());
    try
    {
        if (canceled->load(std::memory_order_acquire))
        {
            throw hresult_canceled();
        }
        return session;
    }
    catch (...)
    {
        const auto failure = std::current_exception();
        const auto failedStage = stage;
        stage = Controller::MatterCameraStage::SessionEnd;
        ownership->Release();
        stage = failedStage;
        std::rethrow_exception(failure);
    }
}

Controller::MatterCameraFailureCategory CameraFailureCategory(
    Controller::MatterCameraStage stage, int32_t result, bool hasNativeError, uint32_t nativeError)
{
    using Category = Controller::MatterCameraFailureCategory;
    if (hasNativeError)
    {
        if (nativeError == CHIP_IM_GLOBAL_STATUS(DynamicConstraintError).AsInteger())
        {
            return Category::DynamicConstraintError;
        }
        if (nativeError == CHIP_IM_GLOBAL_STATUS(ConstraintError).AsInteger())
        {
            return Category::ConstraintError;
        }
        if (nativeError == CHIP_ERROR_TIMEOUT.AsInteger())
        {
            return Category::Timeout;
        }
        if (nativeError == CHIP_ERROR_BUSY.AsInteger() || nativeError == CHIP_ERROR_NO_MEMORY.AsInteger() ||
            nativeError == CHIP_IM_GLOBAL_STATUS(ResourceExhausted).AsInteger())
        {
            return Category::ResourceExhausted;
        }
        if (nativeError == CHIP_ERROR_ACCESS_DENIED.AsInteger() ||
            nativeError == CHIP_IM_GLOBAL_STATUS(UnsupportedAccess).AsInteger())
        {
            return Category::AccessDenied;
        }
        if (nativeError == CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE.AsInteger() ||
            nativeError == CHIP_IM_GLOBAL_STATUS(UnsupportedCluster).AsInteger() ||
            nativeError == CHIP_IM_GLOBAL_STATUS(UnsupportedCommand).AsInteger() ||
            nativeError == CHIP_IM_GLOBAL_STATUS(UnsupportedAttribute).AsInteger())
        {
            return Category::Unsupported;
        }
        if (nativeError == CHIP_ERROR_NOT_CONNECTED.AsInteger() || nativeError == CHIP_ERROR_CONNECTION_ABORTED.AsInteger() ||
            nativeError == CHIP_ERROR_CONNECTION_CLOSED_UNEXPECTEDLY.AsInteger())
        {
            return Category::ConnectionFailure;
        }
        if (nativeError == CHIP_ERROR_INCORRECT_STATE.AsInteger() || nativeError == CHIP_ERROR_NOT_FOUND.AsInteger() ||
            nativeError == CHIP_IM_GLOBAL_STATUS(InvalidInState).AsInteger())
        {
            return Category::InvalidState;
        }
        if (nativeError == CHIP_ERROR_INVALID_ARGUMENT.AsInteger())
        {
            return stage == Controller::MatterCameraStage::Validation ? Category::InvalidInput : Category::MalformedData;
        }
        if (CHIP_ERROR(nativeError).IsIMStatus())
        {
            return Category::CommandRejected;
        }
        return Category::Unknown;
    }
    if (result == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
    {
        return Category::Timeout;
    }
    if (result == E_ACCESSDENIED)
    {
        return stage == Controller::MatterCameraStage::Privacy ? Category::PrivacyEnabled : Category::AccessDenied;
    }
    if (result == E_NOTIMPL)
    {
        return Category::Unsupported;
    }
    if (result == E_OUTOFMEMORY)
    {
        return Category::ResourceExhausted;
    }
    if (result == E_INVALIDARG)
    {
        return Category::InvalidInput;
    }
    if (result == E_BOUNDS || result == E_NOINTERFACE)
    {
        return Category::MalformedData;
    }
    if (result == winrt::hresult_illegal_method_call().code())
    {
        return Category::InvalidState;
    }
    return Category::Unknown;
}

} // namespace

Windows::Foundation::IAsyncOperation<Controller::MatterCameraVideoStream>
MatterControllerCamera::AllocateVideoStreamAsync(uint64_t nodeId, uint16_t endpointId,
                                                Controller::MatterCameraVideoStreamOptions options)
{
    auto lifetime = get_strong();
    if (!options)
    {
        throw hresult_invalid_argument(L"Video stream options cannot be null.");
    }
    auto request = get_self<implementation::MatterCameraVideoStreamOptions>(options)->Snapshot();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto ownership = std::make_shared<PendingCameraVideoStream>([runtime = mRuntime, nodeId, endpointId](uint16_t stream) {
        DeallocateCameraVideoStream(runtime, nodeId, endpointId, stream);
    });
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled, ownership] {
        canceled->store(true, std::memory_order_release);
        ownership->Cancel();
    });
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::Validation;
    co_return AllocateCameraVideoStream(mRuntime, nodeId, endpointId, request, canceled, stage, ownership);
}

Windows::Foundation::IAsyncOperation<bool> MatterControllerCamera::IsPrivacyEnabledAsync(uint64_t nodeId, uint16_t endpointId)
{
    auto lifetime = get_strong();
    if (!IsOperationalNodeId(nodeId) || endpointId == kInvalidEndpointId)
    {
        throw hresult_invalid_argument(L"A valid operational node and camera endpoint are required.");
    }
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled] { canceled->store(true, std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    co_await resume_background();
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    auto attributes = DescriptorNumericList<uint32_t>(
        mRuntime->ReadAttribute(nodeId, endpointId, Id, Attributes::AttributeList::Id, canceled, deadline));
    co_return CameraPrivacyEnabled(mRuntime, nodeId, endpointId, single_threaded_vector(std::move(attributes)).GetView(),
                                  canceled, deadline);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraVideoStreamAllocationResult>
MatterControllerCamera::TryAllocateVideoStreamAsync(uint64_t nodeId, uint16_t endpointId,
                                                   Controller::MatterCameraVideoStreamOptions options)
{
    auto lifetime = get_strong();
    auto stage = Controller::MatterCameraStage::Validation;
    chip::app::Clusters::CameraAvStreamManagement::Commands::VideoStreamAllocate::Type request;
    auto validation = RunCameraOperation(stage, [&] {
        if (!options)
        {
            throw hresult_invalid_argument(L"Video stream options cannot be null.");
        }
        request = get_self<implementation::MatterCameraVideoStreamOptions>(options)->Snapshot();
    });
    if (!validation.Succeeded())
    {
        co_return winrt::make<implementation::MatterCameraVideoStreamAllocationResult>(validation.Failure());
    }
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto ownership = std::make_shared<PendingCameraVideoStream>([runtime = mRuntime, nodeId, endpointId](uint16_t stream) {
        DeallocateCameraVideoStream(runtime, nodeId, endpointId, stream);
    });
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled, ownership] {
        canceled->store(true, std::memory_order_release);
        ownership->Cancel();
    });
    co_await resume_background();
    Controller::MatterCameraVideoStream stream{ nullptr };
    Controller::MatterCameraVideoStreamAllocationResult allocation{ nullptr };
    auto result = RunCameraOperation(stage, [&] {
        stream = AllocateCameraVideoStream(mRuntime, nodeId, endpointId, request, canceled, stage, ownership);
        try
        {
            allocation = winrt::make<implementation::MatterCameraVideoStreamAllocationResult>(stream);
        }
        catch (...)
        {
            const auto failedStage = stage;
            stage = Controller::MatterCameraStage::StreamDeallocation;
            ownership->Release();
            stage = failedStage;
            throw;
        }
    });
    co_return result.Succeeded() ? allocation
                               : winrt::make<implementation::MatterCameraVideoStreamAllocationResult>(result.Failure());
}

Windows::Foundation::IAsyncAction MatterControllerCamera::DeallocateVideoStreamAsync(
    uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId)
{
    auto lifetime = get_strong();
    co_await resume_background();
    DeallocateCameraVideoStream(mRuntime, nodeId, endpointId, videoStreamId);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraOperationResult>
MatterControllerCamera::TryDeallocateVideoStreamAsync(uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId)
{
    auto lifetime = get_strong();
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::StreamDeallocation;
    co_return RunCameraOperation(stage, [&] { DeallocateCameraVideoStream(mRuntime, nodeId, endpointId, videoStreamId); });
}

MatterCameraFailure::MatterCameraFailure(Controller::MatterCameraStage stage, int32_t result, bool hasNativeError,
                                        uint32_t nativeError, hstring message) :
    mStage(stage), mCategory(CameraFailureCategory(stage, result, hasNativeError, nativeError)), mHResult(result),
    mHasNativeError(hasNativeError), mNativeError(nativeError), mMessage(std::move(message))
{}

bool MatterCameraFailure::Retryable() const
{
    return mCategory == Controller::MatterCameraFailureCategory::Timeout ||
        mCategory == Controller::MatterCameraFailureCategory::ConnectionFailure ||
        mCategory == Controller::MatterCameraFailureCategory::ResourceExhausted;
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraSession>
MatterControllerCamera::StartWithAllocatedVideoStreamAsync(uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId,
                                                           hstring offerSdp)
{
    auto lifetime = get_strong();
    const auto sdp = to_string(offerSdp);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto ownership = std::make_shared<PendingCameraSession>([runtime = mRuntime](uint64_t handle) { runtime->CloseCamera(handle); });
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled, ownership] {
        canceled->store(true, std::memory_order_release);
        ownership->Cancel();
    });
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::Validation;
    co_return StartPreparedCamera(mRuntime, nodeId, endpointId, videoStreamId, sdp, canceled, stage, ownership);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraStartResult>
MatterControllerCamera::TryStartWithAllocatedVideoStreamAsync(uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId,
                                                              hstring offerSdp)
{
    auto lifetime = get_strong();
    const auto sdp = to_string(offerSdp);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto ownership = std::make_shared<PendingCameraSession>([runtime = mRuntime](uint64_t handle) { runtime->CloseCamera(handle); });
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled, ownership] {
        canceled->store(true, std::memory_order_release);
        ownership->Cancel();
    });
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::Validation;
    Controller::MatterCameraStartResult result{ nullptr };
    try
    {
        auto session = StartPreparedCamera(mRuntime, nodeId, endpointId, videoStreamId, sdp, canceled, stage, ownership);
        try
        {
            result = winrt::make<implementation::MatterCameraStartResult>(session);
        }
        catch (...)
        {
            stage = Controller::MatterCameraStage::SessionEnd;
            ownership->Release();
            throw;
        }
    }
    catch (hresult_canceled const &)
    {
        throw;
    }
    catch (ChipOperationError const & error)
    {
        result = winrt::make<implementation::MatterCameraStartResult>(
            winrt::make<implementation::MatterCameraFailure>(stage, int32_t(error.code()), true,
                error.NativeError().AsInteger(), error.message()));
    }
    catch (hresult_error const & error)
    {
        result = winrt::make<implementation::MatterCameraStartResult>(
            winrt::make<implementation::MatterCameraFailure>(stage, int32_t(error.code()), false, uint32_t(0), error.message()));
    }
    co_return result;
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraStartResult>
MatterControllerCamera::TryStartWithIceServersAsync(uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId,
    hstring offerSdp, Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceServer> iceServers)
{
    auto lifetime = get_strong();
    auto stage = Controller::MatterCameraStage::Validation;
    std::vector<Controller::MatterCameraIceServer> servers;
    auto validation = RunCameraOperation(stage, [&]() {
        if (!iceServers || iceServers.Size() > 4)
        {
            throw hresult_invalid_argument(L"An explicit list of zero to four ICE servers is required.");
        }
        for (auto const & server : iceServers)
        {
            if (!server)
            {
                throw hresult_invalid_argument(L"ICE servers cannot contain null entries.");
            }
            servers.push_back(server);
        }
    });
    if (!validation.Succeeded())
    {
        co_return winrt::make<implementation::MatterCameraStartResult>(validation.Failure());
    }
    const auto sdp = to_string(offerSdp);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto ownership = std::make_shared<PendingCameraSession>([runtime = mRuntime](uint64_t handle) { runtime->CloseCamera(handle); });
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled, ownership] {
        canceled->store(true, std::memory_order_release);
        ownership->Cancel();
    });
    co_await resume_background();
    Controller::MatterCameraStartResult result{ nullptr };
    auto operation = RunCameraOperation(stage, [&]() {
        auto session = StartPreparedCamera(mRuntime, nodeId, endpointId, videoStreamId, sdp, canceled, stage, ownership, servers);
        try
        {
            result = winrt::make<implementation::MatterCameraStartResult>(std::move(session));
        }
        catch (...)
        {
            stage = Controller::MatterCameraStage::SessionEnd;
            ownership->Release();
            throw;
        }
    });
    if (!operation.Succeeded())
    {
        co_return winrt::make<implementation::MatterCameraStartResult>(operation.Failure());
    }
    co_return result;
}

namespace {

void ValidateCameraSelectedAudio(Controller::MatterCameraAudioCapabilities const & capabilities,
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraAudioStream> const & streams, uint16_t id)
{
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    auto found = std::find_if(streams.begin(), streams.end(), [&](auto const & stream) { return stream.StreamId() == id; });
    if (found == streams.end() || !(*found).IsLiveView())
    {
        throw hresult_invalid_argument(L"The selected audio ID is not an allocated LiveView stream.");
    }
    auto stream = *found;
    Commands::AudioStreamAllocate::Type request;
    request.streamUsage = chip::app::Clusters::Globals::StreamUsageEnum::kLiveView;
    request.audioCodec = static_cast<AudioCodecEnum>(stream.AudioCodec());
    request.channelCount = stream.Channels();
    request.sampleRate = stream.SampleRate();
    request.bitRate = stream.BitRate();
    request.bitDepth = stream.BitDepth();
    if ((request.audioCodec != AudioCodecEnum::kOpus && request.audioCodec != AudioCodecEnum::kAacLc) ||
        (request.bitDepth != 8 && request.bitDepth != 16 && request.bitDepth != 24 && request.bitDepth != 32) ||
        request.bitRate > capabilities.MaximumNetworkBandwidth())
    {
        throw hresult_not_implemented(L"The selected audio format is unsupported or exceeds advertised bandwidth.");
    }
    get_self<implementation::MatterCameraAudioFormatCapabilities>(capabilities.Microphone())->Validate(request);
}

void ValidateCameraSelectedVideo(
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraVideoStream> const & streams, uint16_t id)
{
    const auto stream = FindAllocatedLiveViewStream(streams, id);
    if (!stream)
    {
        throw hresult_invalid_argument(L"The selected video ID is not an allocated LiveView stream.");
    }
    if (stream.VideoCodec() != uint8_t(chip::app::Clusters::CameraAvStreamManagement::VideoCodecEnum::kH264))
    {
        throw hresult_not_implemented(L"The selected video codec is not supported by this camera adapter.");
    }
    if (stream.MinimumWidth() == 0 || stream.MinimumHeight() == 0 || stream.MinimumFrameRate() == 0 ||
        stream.MinimumBitRate() == 0 || stream.KeyFrameInterval() == 0)
    {
        throw hresult_error(E_BOUNDS, L"The selected video stream has zero format bounds.");
    }
}

Controller::MatterCameraSession StartPreparedCameraSession(std::shared_ptr<ControllerRuntime> const & runtime,
    uint64_t node, uint16_t endpoint, Controller::MatterCameraStreamSelection const & video,
    Controller::MatterCameraStreamSelection const & audio, std::string const & sdp,
    std::vector<Controller::MatterCameraIceServer> const & servers, std::shared_ptr<std::atomic_bool> const & canceled,
    Controller::MatterCameraStage & stage, std::shared_ptr<PendingCameraSession> const & ownership)
{
    using Kind = Controller::MatterCameraStreamSelectionKind;
    stage = Controller::MatterCameraStage::Validation;
    if (!IsOperationalNodeId(node) || endpoint == kInvalidEndpointId ||
        !video || !audio || (video.Kind() == Kind::Absent && audio.Kind() == Kind::Absent) ||
        sdp.empty() || sdp.size() > 16384 || sdp.find('\0') != std::string::npos)
    {
        throw hresult_invalid_argument(L"Explicit selections for both media types, at least one selected medium, and bounded SDP are required.");
    }
    runtime->EnsureCameraSignalingEnabled();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    auto capabilities = ReadCameraAudioCapabilities(runtime, node, endpoint, canceled, deadline, stage);
    if (audio.Kind() != Kind::Absent)
    {
        RequireListening(capabilities, stage);
        stage = Controller::MatterCameraStage::StreamSelection;
        auto usages = DescriptorNumericList<uint8_t>(runtime->ReadAttribute(node, endpoint,
            chip::app::Clusters::CameraAvStreamManagement::Id,
            chip::app::Clusters::CameraAvStreamManagement::Attributes::SupportedStreamUsages::Id, canceled, deadline));
        if (!CameraAdvertises(usages, uint8_t(chip::app::Clusters::Globals::StreamUsageEnum::kLiveView)))
        {
            throw hresult_not_implemented(L"The camera does not advertise LiveView audio usage.");
        }
    }
    if (video.Kind() != Kind::Absent && !capabilities.Camera().SupportsClientOffer())
    {
        stage = Controller::MatterCameraStage::Capabilities;
        throw hresult_not_implemented(L"The camera does not advertise video and the complete client-offer path.");
    }
    stage = Controller::MatterCameraStage::Privacy;
    ReadCameraPrivacy(runtime, node, capabilities.Camera(), canceled, deadline);
    stage = Controller::MatterCameraStage::StreamSelection;
    if (video.Kind() != Kind::Absent &&
        !CameraAdvertises(capabilities.Camera().StreamManagement().Attributes(),
            chip::app::Clusters::CameraAvStreamManagement::Attributes::AllocatedVideoStreams::Id))
    {
        throw hresult_not_implemented(L"The camera does not advertise video stream readback.");
    }
    if (video.Kind() == Kind::ExplicitId)
    {
        ValidateCameraSelectedVideo(ReadCameraVideoStreams(runtime, node, endpoint, canceled, deadline), video.StreamId().Value());
    }
    if (audio.Kind() == Kind::ExplicitId)
    {
        ValidateCameraSelectedAudio(capabilities, ReadCameraAudioStreams(runtime, node, endpoint, canceled, deadline), audio.StreamId().Value());
    }
    if (canceled->load(std::memory_order_acquire))
    {
        throw hresult_canceled();
    }
    stage = Controller::MatterCameraStage::Signaling;
    auto session = runtime->StartCamera(node, endpoint, video, audio, sdp, canceled, servers);
    try
    {
        ownership->Record(session.Handle());
        stage = Controller::MatterCameraStage::StreamSelection;
        auto selected = runtime->SelectedCameraStreams(session.Handle());
        if (selected.first)
        {
            ValidateCameraSelectedVideo(ReadCameraVideoStreams(runtime, node, endpoint, canceled, deadline), *selected.first);
        }
        if (selected.second)
        {
            ValidateCameraSelectedAudio(capabilities, ReadCameraAudioStreams(runtime, node, endpoint, canceled, deadline), *selected.second);
        }
        stage = Controller::MatterCameraStage::Privacy;
        ReadCameraPrivacy(runtime, node, capabilities.Camera(), canceled, deadline);
        if (canceled->load(std::memory_order_acquire))
        {
            throw hresult_canceled();
        }
        return session;
    }
    catch (...)
    {
        const auto failure = std::current_exception();
        const auto failedStage = stage;
        stage = Controller::MatterCameraStage::SessionEnd;
        ownership->Release();
        stage = failedStage;
        std::rethrow_exception(failure);
    }
}

std::vector<Controller::MatterCameraIceServer> CameraSessionIceServers(
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceServer> const & servers)
{
    if (!servers || servers.Size() > 4)
    {
        throw hresult_invalid_argument(L"An explicit list of zero to four ICE servers is required.");
    }
    std::vector<Controller::MatterCameraIceServer> result;
    for (auto const & server : servers)
    {
        if (!server)
        {
            throw hresult_invalid_argument(L"ICE server entries cannot be null.");
        }
        result.push_back(server);
    }
    return result;
}

std::pair<uint32_t, Windows::Foundation::Collections::IPropertySet> CameraAudioControlArguments(
    Controller::MatterCameraAudioCapabilities const & capabilities, bool speaker, bool volume, uint8_t value)
{
    using namespace chip::app::Clusters::CameraAvStreamManagement;
    RequireCameraAudioAdapter(capabilities.Camera());
    auto format = speaker ? capabilities.Speaker() : capabilities.Microphone();
    auto muted = speaker ? capabilities.SpeakerMuted() : capabilities.MicrophoneMuted();
    auto level = speaker ? capabilities.SpeakerVolume() : capabilities.MicrophoneVolume();
    auto minimum = speaker ? capabilities.SpeakerMinimumVolume() : capabilities.MicrophoneMinimumVolume();
    auto maximum = speaker ? capabilities.SpeakerMaximumVolume() : capabilities.MicrophoneMaximumVolume();
    if (!format || (volume ? (!level || !minimum || !maximum) : !muted))
    {
        throw hresult_not_implemented(L"The camera does not advertise this microphone/speaker control.");
    }
    if (volume && (minimum.Value() > maximum.Value() || level.Value() < minimum.Value() || level.Value() > maximum.Value()))
    {
        throw hresult_error(E_BOUNDS, L"The camera reports inconsistent volume bounds.");
    }
    if (volume && (value < minimum.Value() || value > maximum.Value()))
    {
        throw hresult_invalid_argument(L"Volume is outside the camera's advertised minimum and maximum.");
    }
    if (!volume && value == 0 && capabilities.PrivacyEnabled())
    {
        throw hresult_error(E_ACCESSDENIED, L"Camera privacy mode prevents unmuting.");
    }
    const auto attribute = volume ? (speaker ? Attributes::SpeakerVolumeLevel::Id : Attributes::MicrophoneVolumeLevel::Id)
                                 : (speaker ? Attributes::SpeakerMuted::Id : Attributes::MicrophoneMuted::Id);
    Windows::Foundation::Collections::PropertySet data;
    data.Insert(L"value", volume ? box_value(value) : box_value(value != 0));
    return { attribute, data };
}

void WriteCameraAudioControl(std::shared_ptr<ControllerRuntime> const & runtime, uint64_t node, uint16_t endpoint,
    bool speaker, bool volume, uint8_t value)
{
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto stage = Controller::MatterCameraStage::Validation;
    auto capabilities = ReadCameraAudioCapabilities(runtime, node, endpoint, canceled,
        std::chrono::steady_clock::now() + std::chrono::seconds(120), stage);
    auto [attribute, data] = CameraAudioControlArguments(capabilities, speaker, volume, value);
    runtime->WriteAttribute(node, endpoint, chip::app::Clusters::CameraAvStreamManagement::Id, attribute, data);
}

} // namespace

Windows::Foundation::IAsyncOperation<Controller::MatterCameraSession>
MatterControllerCamera::StartSessionAsync(uint64_t nodeId, uint16_t endpointId,
    Controller::MatterCameraStreamSelection video, Controller::MatterCameraStreamSelection audio, hstring offer,
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceServer> servers)
{
    auto lifetime = get_strong();
    auto values = CameraSessionIceServers(servers);
    auto sdp = to_string(offer);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto ownership = std::make_shared<PendingCameraSession>([runtime = mRuntime](uint64_t handle) { runtime->CloseCamera(handle); });
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled, ownership] {
        canceled->store(true, std::memory_order_release);
        ownership->Cancel();
    });
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::Validation;
    co_return StartPreparedCameraSession(mRuntime, nodeId, endpointId, video, audio, sdp, values, canceled, stage, ownership);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraStartResult>
MatterControllerCamera::TryStartSessionAsync(uint64_t nodeId, uint16_t endpointId,
    Controller::MatterCameraStreamSelection video, Controller::MatterCameraStreamSelection audio, hstring offer,
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceServer> servers)
{
    auto lifetime = get_strong();
    auto stage = Controller::MatterCameraStage::Validation;
    std::vector<Controller::MatterCameraIceServer> values;
    auto validation = RunCameraOperation(stage, [&] { values = CameraSessionIceServers(servers); });
    if (!validation.Succeeded())
    {
        co_return winrt::make<implementation::MatterCameraStartResult>(validation.Failure());
    }
    auto sdp = to_string(offer);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto ownership = std::make_shared<PendingCameraSession>([runtime = mRuntime](uint64_t handle) { runtime->CloseCamera(handle); });
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled, ownership] {
        canceled->store(true, std::memory_order_release);
        ownership->Cancel();
    });
    co_await resume_background();
    Controller::MatterCameraStartResult result{ nullptr };
    auto operation = RunCameraOperation(stage, [&] {
        auto session = StartPreparedCameraSession(mRuntime, nodeId, endpointId, video, audio, sdp, values, canceled, stage, ownership);
        try
        {
            result = winrt::make<implementation::MatterCameraStartResult>(session);
        }
        catch (...)
        {
            stage = Controller::MatterCameraStage::SessionEnd;
            ownership->Release();
            throw;
        }
    });
    co_return operation.Succeeded() ? result : winrt::make<implementation::MatterCameraStartResult>(operation.Failure());
}

Windows::Foundation::IAsyncAction MatterControllerCamera::WriteMicrophoneMuteAsync(uint64_t nodeId, uint16_t endpointId, bool muted)
{
    auto lifetime = get_strong();
    co_await resume_background();
    WriteCameraAudioControl(mRuntime, nodeId, endpointId, false, false, uint8_t(muted));
}

Windows::Foundation::IAsyncAction MatterControllerCamera::WriteSpeakerMuteAsync(uint64_t nodeId, uint16_t endpointId, bool muted)
{
    auto lifetime = get_strong();
    co_await resume_background();
    WriteCameraAudioControl(mRuntime, nodeId, endpointId, true, false, uint8_t(muted));
}

Windows::Foundation::IAsyncAction MatterControllerCamera::WriteMicrophoneVolumeAsync(uint64_t nodeId, uint16_t endpointId, uint8_t level)
{
    auto lifetime = get_strong();
    co_await resume_background();
    WriteCameraAudioControl(mRuntime, nodeId, endpointId, false, true, level);
}

Windows::Foundation::IAsyncAction MatterControllerCamera::WriteSpeakerVolumeAsync(uint64_t nodeId, uint16_t endpointId, uint8_t level)
{
    auto lifetime = get_strong();
    co_await resume_background();
    WriteCameraAudioControl(mRuntime, nodeId, endpointId, true, true, level);
}

MatterEndpointDescriptor::MatterEndpointDescriptor(uint16_t endpointId, std::vector<uint16_t> parts,
                                                    std::vector<uint32_t> servers, std::vector<uint32_t> clients,
                                                    std::vector<Controller::MatterDeviceType> types) :
    mEndpointId(endpointId), mParts(single_threaded_vector(std::move(parts)).GetView()),
    mServers(single_threaded_vector(std::move(servers)).GetView()),
    mClients(single_threaded_vector(std::move(clients)).GetView()),
    mTypes(single_threaded_vector(std::move(types)).GetView())
{}

bool MatterEndpointDescriptor::HasCameraClusters() const
{
    return std::find(mServers.begin(), mServers.end(), chip::app::Clusters::CameraAvStreamManagement::Id) != mServers.end() &&
        std::find(mServers.begin(), mServers.end(), chip::app::Clusters::WebRTCTransportProvider::Id) != mServers.end();
}

MatterControllerCameraDiscovery::MatterControllerCameraDiscovery(Controller::MatterController controller)
{
    if (!controller)
    {
        throw hresult_invalid_argument(L"controller cannot be null.");
    }
    mRuntime = get_self<implementation::MatterController>(controller)->Runtime();
}

Windows::Foundation::IAsyncOperation<Controller::MatterEndpointDescriptor>
MatterControllerCameraDiscovery::ReadDescriptorAsync(uint64_t nodeId, uint16_t endpointId)
{
    if (!IsOperationalNodeId(nodeId) || endpointId == kInvalidEndpointId)
    {
        throw hresult_invalid_argument(L"A valid operational node and endpoint are required.");
    }
    auto runtime = mRuntime;
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled]() { canceled->store(true, std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    co_await resume_background();
    co_return ReadEndpointDescriptor(runtime, nodeId, endpointId, canceled, deadline);
}

namespace {

Windows::Foundation::Collections::IVectorView<Controller::MatterEndpointDescriptor> DiscoverCameraEndpoints(
    std::shared_ptr<ControllerRuntime> const & runtime, uint64_t nodeId,
    std::shared_ptr<std::atomic_bool> const & canceled, Controller::MatterCameraStage & stage)
{
    stage = Controller::MatterCameraStage::Validation;
    if (canceled->load(std::memory_order_acquire))
    {
        throw hresult_canceled();
    }
    if (!IsOperationalNodeId(nodeId))
    {
        throw hresult_invalid_argument(L"A valid operational node is required.");
    }
    stage = Controller::MatterCameraStage::Discovery;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    std::vector<uint16_t> endpoints{ kRootEndpointId };
    std::vector<Controller::MatterEndpointDescriptor> cameras;
    for (size_t i = 0; i < endpoints.size(); ++i)
    {
        auto descriptor = ReadEndpointDescriptor(runtime, nodeId, endpoints[i], canceled, deadline);
        for (const auto endpoint : descriptor.PartsList())
        {
            if (endpoint == kInvalidEndpointId || endpoint == endpoints[i])
            {
                throw hresult_error(E_BOUNDS, L"The Descriptor PartsList contains an invalid endpoint.");
            }
            if (std::find(endpoints.begin(), endpoints.end(), endpoint) == endpoints.end())
            {
                if (endpoints.size() == kMaximumDiscoveredEndpoints)
                {
                    throw hresult_error(E_BOUNDS, L"Camera discovery exceeds the supported endpoint limit.");
                }
                endpoints.push_back(endpoint);
            }
        }
        if (descriptor.HasCameraClusters())
        {
            cameras.push_back(std::move(descriptor));
        }
    }
    return single_threaded_vector(std::move(cameras)).GetView();
}

} // namespace

Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::MatterEndpointDescriptor>>
MatterControllerCameraDiscovery::DiscoverCamerasAsync(uint64_t nodeId)
{
    auto lifetime = get_strong();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled]() { canceled->store(true, std::memory_order_release); });
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::Validation;
    co_return DiscoverCameraEndpoints(mRuntime, nodeId, canceled, stage);
}

Windows::Foundation::IAsyncOperation<Controller::MatterCameraDiscoveryResult>
MatterControllerCameraDiscovery::TryDiscoverCamerasAsync(uint64_t nodeId)
{
    auto lifetime = get_strong();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = co_await get_cancellation_token();
    cancellation.callback([canceled]() { canceled->store(true, std::memory_order_release); });
    co_await resume_background();
    auto stage = Controller::MatterCameraStage::Validation;
    Windows::Foundation::Collections::IVectorView<Controller::MatterEndpointDescriptor> cameras{ nullptr };
    auto operation = RunCameraOperation(stage, [&]() { cameras = DiscoverCameraEndpoints(mRuntime, nodeId, canceled, stage); });
    if (!operation.Succeeded())
    {
        co_return winrt::make<implementation::MatterCameraDiscoveryResult>(operation.Failure());
    }
    co_return winrt::make<implementation::MatterCameraDiscoveryResult>(std::move(cameras));
}

MatterControllerRecovery::MatterControllerRecovery(Controller::MatterController controller)
{
    if (!controller)
    {
        throw hresult_invalid_argument(L"controller cannot be null.");
    }
    mRuntime = get_self<implementation::MatterController>(controller)->Runtime();
}

MatterControllerNetworkCommissioning::MatterControllerNetworkCommissioning(Controller::MatterController controller)
{
    if (!controller)
    {
        throw hresult_invalid_argument(L"controller cannot be null.");
    }
    mRuntime = get_self<implementation::MatterController>(controller)->Runtime();
}

Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
MatterControllerNetworkCommissioning::CommissionBleAsync(Controller::BleNetworkCommissioningParameters parameters)
{
    if (!parameters)
    {
        throw hresult_invalid_argument(L"parameters cannot be null.");
    }

    auto wiFi  = parameters.WiFi();
    auto thread = parameters.Thread();
    if (static_cast<bool>(wiFi) == static_cast<bool>(thread))
    {
        throw hresult_invalid_argument(L"Provide exactly one Wi-Fi or Thread credential set.");
    }

    std::optional<WiFiCredentials> nativeWiFi;
    ByteSpan threadDataset;
    if (wiFi)
    {
        auto implementation = get_self<implementation::WiFiNetworkCredentials>(wiFi);
        auto const & ssid       = implementation->Ssid();
        auto const & passphrase = implementation->Passphrase();
        nativeWiFi.emplace(ByteSpan(ssid.data(), ssid.size()), ByteSpan(passphrase.data(), passphrase.size()));
    }
    else
    {
        auto const & dataset = get_self<implementation::ThreadNetworkCredentials>(thread)->OperationalDataset();
        threadDataset        = ByteSpan(dataset.data(), dataset.size());
    }

    uint64_t nodeId        = parameters.NodeId();
    uint32_t setupPinCode  = parameters.SetupPinCode();
    uint16_t discriminator = parameters.LongDiscriminator();
    co_await resume_background();
    co_return mRuntime->Commission(nodeId, setupPinCode, discriminator, true, {}, nativeWiFi, threadDataset);
}

Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::MatterNetworkInterface>>
MatterNetworkInterfaceProvider::GetEligibleNetworkInterfacesAsync()
{
    co_await resume_background();
    std::vector<Controller::MatterNetworkInterface> interfaces;
    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        Inet::InterfaceId id = iterator.GetInterfaceId();
        if (!id.IsPresent() || iterator.IsLoopback())
        {
            continue;
        }

        char nameBuffer[Inet::InterfaceId::kMaxIfNameLength] = {};
        CheckChipError(iterator.GetInterfaceName(nameBuffer, sizeof(nameBuffer)), L"Read network interface name");

        Inet::InterfaceType nativeType = Inet::InterfaceType::Unknown;
        CheckChipError(iterator.GetInterfaceType(nativeType), L"Read network interface type");
        bool supportsIpv6 = false;
        for (Inet::InterfaceAddressIterator addressIterator; addressIterator.HasCurrent(); addressIterator.Next())
        {
            Inet::IPAddress address;
            if (addressIterator.GetInterfaceId() == id && addressIterator.GetAddress(address) == CHIP_NO_ERROR && address.IsIPv6())
            {
                supportsIpv6 = true;
                break;
            }
        }

        std::string name = nameBuffer;
        std::string lowerName(name);
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        bool isVirtual = lowerName.find("virtual") != std::string::npos || lowerName.find("vethernet") != std::string::npos ||
            lowerName.find("hyper-v") != std::string::npos || lowerName.find("vpn") != std::string::npos;
        interfaces.push_back(winrt::make<implementation::MatterNetworkInterface>(
            id.GetPlatformInterface(), id.GetInterfaceIndex(), to_hstring(name),
            static_cast<Controller::MatterNetworkInterfaceType>(nativeType), iterator.IsUp(), supportsIpv6,
            iterator.SupportsMulticast(), isVirtual));
    }
    co_return single_threaded_vector(std::move(interfaces)).GetView();
}

MatterControllerCommissioning::MatterControllerCommissioning(Controller::MatterController controller)
{
    if (!controller)
    {
        throw hresult_invalid_argument(L"controller cannot be null.");
    }
    mRuntime = get_self<implementation::MatterController>(controller)->Runtime();
}

event_token MatterControllerCommissioning::ProgressChanged(
    Windows::Foundation::TypedEventHandler<Controller::MatterControllerCommissioning,
                                           Controller::MatterCommissioningProgress> const & handler)
{
    return mProgressChanged.add(handler);
}

void MatterControllerCommissioning::ProgressChanged(event_token const & token) noexcept
{
    mProgressChanged.remove(token);
}

void MatterControllerCommissioning::Publish(Controller::MatterCommissioningProgress const & progress)
{
    bool scheduleDrain = false;
    {
        std::scoped_lock lock(mProgressMutex);
        mPendingProgress.push_back(progress);
        if (!mProgressDrainScheduled)
        {
            mProgressDrainScheduled = true;
            scheduleDrain            = true;
        }
    }
    if (scheduleDrain)
    {
        auto strong = get_strong();
        Windows::System::Threading::ThreadPool::RunAsync(
            [strong = std::move(strong)](Windows::Foundation::IAsyncAction const &) { strong->DrainProgress(); });
    }
}

void MatterControllerCommissioning::DrainProgress()
{
    while (true)
    {
        Controller::MatterCommissioningProgress progress{ nullptr };
        {
            std::scoped_lock lock(mProgressMutex);
            if (mPendingProgress.empty())
            {
                mProgressDrainScheduled = false;
                mProgressCondition.notify_all();
                return;
            }
            progress = mPendingProgress.front();
            mPendingProgress.erase(mPendingProgress.begin());
        }
        try
        {
            mProgressChanged(*this, progress);
        }
        catch (hresult_error const & error)
        {
            ChipLogError(Controller, "Commissioning progress handler failed: 0x%08lx",
                         static_cast<unsigned long>(error.code().value));
        }
    }
}

void MatterControllerCommissioning::WaitForProgressDrain()
{
    std::unique_lock lock(mProgressMutex);
    mProgressCondition.wait(lock, [this]() { return !mProgressDrainScheduled && mPendingProgress.empty(); });
}

Windows::Foundation::IAsyncOperation<Controller::MatterCommissioningResult>
MatterControllerCommissioning::CommissionBleAsync(Controller::BleNetworkCommissioningParameters parameters,
                                                   Controller::MatterNetworkInterfaceSelection interfaceSelection)
{
    auto lifetime = get_strong();
    if (!parameters)
    {
        throw hresult_invalid_argument(L"parameters cannot be null.");
    }
    if (!interfaceSelection)
    {
        interfaceSelection = winrt::make<implementation::MatterNetworkInterfaceSelection>(
            Controller::MatterNetworkInterfaceSelectionMode::Automatic, 0);
    }

    auto wiFi   = parameters.WiFi();
    auto thread = parameters.Thread();
    if (static_cast<bool>(wiFi) == static_cast<bool>(thread))
    {
        throw hresult_invalid_argument(L"Provide exactly one Wi-Fi or Thread credential set.");
    }

    std::optional<WiFiCredentials> nativeWiFi;
    ByteSpan threadDataset;
    if (wiFi)
    {
        auto implementation     = get_self<implementation::WiFiNetworkCredentials>(wiFi);
        auto const & ssid       = implementation->Ssid();
        auto const & passphrase = implementation->Passphrase();
        nativeWiFi.emplace(ByteSpan(ssid.data(), ssid.size()), ByteSpan(passphrase.data(), passphrase.size()));
    }
    else
    {
        auto const & dataset = get_self<implementation::ThreadNetworkCredentials>(thread)->OperationalDataset();
        threadDataset        = ByteSpan(dataset.data(), dataset.size());
    }

    AddressResolve::InterfaceSelection nativeSelection;
    nativeSelection.interfaceId = Inet::InterfaceId(interfaceSelection.InterfaceId());
    switch (interfaceSelection.Mode())
    {
    case Controller::MatterNetworkInterfaceSelectionMode::Automatic:
        nativeSelection.mode = AddressResolve::InterfaceSelectionMode::kAutomatic;
        break;
    case Controller::MatterNetworkInterfaceSelectionMode::PreferSpecifiedInterface:
        nativeSelection.mode = AddressResolve::InterfaceSelectionMode::kPrefer;
        break;
    case Controller::MatterNetworkInterfaceSelectionMode::RequireSpecifiedInterface:
        nativeSelection.mode = AddressResolve::InterfaceSelectionMode::kRequire;
        break;
    default:
        throw hresult_invalid_argument(L"Unknown network interface selection mode.");
    }

    hstring interfaceName;
    if (nativeSelection.mode != AddressResolve::InterfaceSelectionMode::kAutomatic)
    {
        bool found             = false;
        bool connected         = false;
        bool supportsMulticast = false;
        bool supportsIpv6      = false;
        for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
        {
            if (iterator.GetInterfaceId() != nativeSelection.interfaceId)
            {
                continue;
            }
            found             = true;
            connected         = iterator.IsUp();
            supportsMulticast = iterator.SupportsMulticast();
            char nameBuffer[Inet::InterfaceId::kMaxIfNameLength] = {};
            if (iterator.GetInterfaceName(nameBuffer, sizeof(nameBuffer)) == CHIP_NO_ERROR)
            {
                interfaceName = to_hstring(nameBuffer);
            }
            break;
        }
        if (found)
        {
            for (Inet::InterfaceAddressIterator iterator; iterator.HasCurrent(); iterator.Next())
            {
                Inet::IPAddress address;
                if (iterator.GetInterfaceId() == nativeSelection.interfaceId &&
                    iterator.GetAddress(address) == CHIP_NO_ERROR && address.IsIPv6())
                {
                    supportsIpv6 = true;
                    break;
                }
            }
        }
        if (nativeSelection.mode == AddressResolve::InterfaceSelectionMode::kRequire)
        {
            if (!found)
            {
                throw hresult_invalid_argument(L"PreferredInterfaceNotFound");
            }
            if (!connected)
            {
                throw hresult_invalid_argument(L"PreferredInterfaceDisconnected");
            }
            if (!supportsIpv6)
            {
                throw hresult_invalid_argument(L"PreferredInterfaceDoesNotSupportIpv6");
            }
            if (!supportsMulticast)
            {
                throw hresult_invalid_argument(L"PreferredInterfaceDoesNotSupportMulticast");
            }
        }
    }

    struct ProgressState
    {
        std::mutex mutex;
        Controller::MatterCommissioningStage lastCompleted = Controller::MatterCommissioningStage::Unknown;
        Controller::MatterCommissioningStage active        = Controller::MatterCommissioningStage::SearchingForDevice;
        int32_t nativeStageId                               = -1;
        int32_t nativeError                                 = 0;
        uint32_t attemptNumber                              = 1;
        uint64_t networkInterfaceId                         = 0;
        hstring networkInterfaceName;
    };

    auto state = std::make_shared<ProgressState>();
    auto start = std::chrono::steady_clock::now();
    uint64_t interfaceId = interfaceSelection.InterfaceId();
    state->networkInterfaceId   = interfaceId;
    state->networkInterfaceName = interfaceName;
    auto selectionMode          = interfaceSelection.Mode();
    auto weak            = get_weak();
    Publish(winrt::make<implementation::MatterCommissioningProgress>(
        Controller::MatterCommissioningStage::SearchingForDevice, Controller::MatterCommissioningStage::Unknown, -1,
        Controller::MatterCommissioningTransport::Bluetooth, Windows::Foundation::TimeSpan::zero(),
        L"Searching for the Matter device over Bluetooth.", L"Searching for device", interfaceId, interfaceName, 1, false));

    CommissioningProgressCallback callback = [weak, state, start, selectionMode](const NativeCommissioningProgress & update) {
        auto self = weak.get();
        if (!self)
        {
            return;
        }
        Controller::MatterCommissioningStage stage = ProjectCommissioningStage(update.stage);
        Controller::MatterCommissioningStage lastCompleted;
        uint32_t attemptNumber;
        uint64_t currentInterfaceId;
        hstring currentInterfaceName;
        {
            std::scoped_lock lock(state->mutex);
            state->active        = stage;
            state->nativeStageId = static_cast<int32_t>(update.stage);
            state->nativeError   = update.error.AsInteger();
            if (update.completed && update.error == CHIP_NO_ERROR)
            {
                state->lastCompleted = stage;
            }
            if (update.retrying)
            {
                ++state->attemptNumber;
                if (selectionMode == Controller::MatterNetworkInterfaceSelectionMode::PreferSpecifiedInterface)
                {
                    state->networkInterfaceId   = 0;
                    state->networkInterfaceName = {};
                }
            }
            lastCompleted        = state->lastCompleted;
            attemptNumber        = state->attemptNumber;
            currentInterfaceId   = state->networkInterfaceId;
            currentInterfaceName = state->networkInterfaceName;
        }
        char const * nativeName = StageToString(update.stage);
        hstring message = to_hstring(nativeName != nullptr && nativeName[0] != '\0' ? nativeName : "Unknown commissioning stage");
        auto elapsed = std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::steady_clock::now() - start);
        self->Publish(winrt::make<implementation::MatterCommissioningProgress>(
            stage, lastCompleted, static_cast<int32_t>(update.stage), ProjectCommissioningTransport(update.stage), elapsed,
            message, message, currentInterfaceId, currentInterfaceName, attemptNumber, update.retrying));
    };

    uint64_t nodeId        = parameters.NodeId();
    uint32_t setupPinCode  = parameters.SetupPinCode();
    uint16_t discriminator = parameters.LongDiscriminator();
    auto cancellationRequested = std::make_shared<std::atomic_bool>(false);
    auto cancellation          = co_await get_cancellation_token();
    cancellation.enable_propagation();
    auto runtime = lifetime->mRuntime;
    cancellation.callback([runtime, cancellationRequested]() noexcept {
        cancellationRequested->store(true, std::memory_order_release);
        runtime->NotifyCommissionCancellation();
    });
    co_await resume_background();
    if (cancellation())
    {
        Publish(winrt::make<implementation::MatterCommissioningProgress>(
            Controller::MatterCommissioningStage::Failed, Controller::MatterCommissioningStage::Unknown, -1,
            Controller::MatterCommissioningTransport::Bluetooth,
            std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::steady_clock::now() - start),
            L"Commissioning was canceled.", L"Commissioning canceled", interfaceId, interfaceName, 1, false));
        WaitForProgressDrain();
        throw hresult_canceled();
    }
    try
    {
        auto node = [&]() {
            try
            {
                return mRuntime->Commission(nodeId, setupPinCode, discriminator, true, {}, nativeWiFi, threadDataset,
                                             MakeOptional(nativeSelection), std::move(callback), cancellationRequested);
            }
            catch (ChipOperationError const & error)
            {
                std::scoped_lock lock(state->mutex);
                state->nativeError = error.NativeError().AsInteger();
                throw;
            }
        }();
        Controller::MatterCommissioningStage lastCompleted;
        uint32_t attemptNumber;
        uint64_t currentInterfaceId;
        hstring currentInterfaceName;
        {
            std::scoped_lock lock(state->mutex);
            lastCompleted        = state->lastCompleted;
            attemptNumber        = state->attemptNumber;
            currentInterfaceId   = state->networkInterfaceId;
            currentInterfaceName = state->networkInterfaceName;
        }
        Publish(winrt::make<implementation::MatterCommissioningProgress>(
            Controller::MatterCommissioningStage::Completed, lastCompleted, -1,
            Controller::MatterCommissioningTransport::OperationalIp,
            std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::steady_clock::now() - start),
            L"Commissioning completed.", L"Commissioning complete", currentInterfaceId, currentInterfaceName, attemptNumber, false));
        auto result = winrt::make<implementation::MatterCommissioningResult>(
            true, Controller::MatterCommissioningOutcome::Succeeded, Controller::MatterCommissioningFailureKind::Unknown,
            Controller::MatterCommissioningStage::Unknown, lastCompleted, -1, 0, L"Commissioning completed.", currentInterfaceId, node);
        WaitForProgressDrain();
        co_return result;
    }
    catch (hresult_error const & error)
    {
        if (cancellationRequested->load(std::memory_order_acquire))
        {
            Controller::MatterCommissioningStage lastCompleted;
            int32_t nativeStageId;
            uint32_t attemptNumber;
            uint64_t currentInterfaceId;
            hstring currentInterfaceName;
            {
                std::scoped_lock lock(state->mutex);
                lastCompleted        = state->lastCompleted;
                nativeStageId        = state->nativeStageId;
                attemptNumber        = state->attemptNumber;
                currentInterfaceId   = state->networkInterfaceId;
                currentInterfaceName = state->networkInterfaceName;
            }
            Publish(winrt::make<implementation::MatterCommissioningProgress>(
                Controller::MatterCommissioningStage::Failed, lastCompleted, nativeStageId,
                Controller::MatterCommissioningTransport::Unknown,
                std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::steady_clock::now() - start),
                L"Commissioning was canceled.", L"Commissioning canceled", currentInterfaceId, currentInterfaceName,
                attemptNumber, false));
            WaitForProgressDrain();
            throw hresult_canceled();
        }
        Controller::MatterCommissioningStage failedStage;
        Controller::MatterCommissioningStage lastCompleted;
        int32_t nativeStageId;
        int32_t nativeError;
        uint32_t attemptNumber;
        uint64_t currentInterfaceId;
        hstring currentInterfaceName;
        {
            std::scoped_lock lock(state->mutex);
            failedStage         = state->active;
            lastCompleted       = state->lastCompleted;
            nativeStageId       = state->nativeStageId;
            nativeError         = state->nativeError;
            attemptNumber       = state->attemptNumber;
            currentInterfaceId   = state->networkInterfaceId;
            currentInterfaceName = state->networkInterfaceName;
        }
        bool timedOut = error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (timedOut)
        {
            nativeError = CHIP_ERROR_TIMEOUT.AsInteger();
        }
        Publish(winrt::make<implementation::MatterCommissioningProgress>(
            Controller::MatterCommissioningStage::Failed, lastCompleted, nativeStageId,
            Controller::MatterCommissioningTransport::Unknown,
            std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::steady_clock::now() - start),
            error.message(), L"Commissioning failed", currentInterfaceId, currentInterfaceName, attemptNumber, false));
        auto result = winrt::make<implementation::MatterCommissioningResult>(
            false, timedOut ? Controller::MatterCommissioningOutcome::TimedOut : Controller::MatterCommissioningOutcome::Failed,
            ClassifyCommissioningFailure(failedStage, timedOut),
            failedStage, lastCompleted, nativeStageId, nativeError, error.message(), currentInterfaceId, nullptr);
        WaitForProgressDrain();
        co_return result;
    }
}

Windows::Foundation::IAsyncAction MatterController::CloseAsync()
{
    std::shared_ptr<ControllerRuntime> runtime;
    {
        std::scoped_lock lock(mRuntimeMutex);
        runtime = std::move(mRuntime);
    }
    if (runtime)
    {
        runtime->Close();
    }
    co_return;
}

} // namespace winrt::Matter::Windows::Controller::implementation
